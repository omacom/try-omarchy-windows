// SPDX-License-Identifier: MIT
// Three GPU staging slots; ordinary frames have no CPU readback or upload.
// Input returns only after its GPU copy retires. Presentation owns its UI thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "binary_capture.h"
#include "compositor_capture_wire.h"
#include "compositor_audit_tiles.h"
#include "sdl-relay-output/compositor_output.h"
#include "input_delivery_diagnostic.h"
#include "staging_reclaim.h"
#include "owned_thread_completion.h"
namespace driver_shared {
class CompositorPresenter {
    using Device = Microsoft::WRL::ComPtr<ID3D12Device>;
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot {
        Resource texture;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> inputAllocator, outputAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> inputCommands, outputCommands;
        bool occupied = false;
        std::uint64_t reusedAfter = 0,bytes=0,generation=0;
    };
    struct Frame { unsigned slot, audit; std::uint64_t sequence; bool final; };
    std::array<Slot,3> slots;
    Device device;
    GpuUsageGuard budget;
    std::shared_ptr<driver_input::Channel> windowInput;
    std::shared_ptr<driver_sdl::Control> sdlControl;
    bool inputDiagnostic=false;
    bool demandStaging = false;
    unsigned stagingTexturesCreated = 0, stagingTexturesReleased = 0;
    std::uint64_t stagingTextureBytes = 0;
    std::uint64_t liveStagingTextureBytes=0;
    unsigned stagingTexturesTrimmed=0;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> inputQueue, outputQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> inputRetired, outputRetired, auditRetired;
    Resource auditReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 auditBytes = 0;
    AuditTilePlan auditPlan{};
    std::uint64_t auditSequence=0;
    std::map<std::uint32_t,Resource> sources;
    std::mutex guard;
    std::condition_variable ready;
    std::deque<Frame> frames;
    std::thread ui;
    HANDLE work = nullptr, inputEvent = nullptr;
    std::atomic<HRESULT> failure{S_OK};
    bool initialized = false, stopping = false;
    bool ending = false, finalized = false;
    HRESULT initialization = E_PENDING;
    LUID selected{};
    driver_bridge::CompositorCaptureDesc layout{};
    std::uint64_t submitted = 0, presented = 0, outputSubmitted = 0;
    unsigned auditAttempts = 0, completedAudits = 0, peakQueued = 0, peakSources = 0;
    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* object,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER result{}; result.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        result.Transition.pResource=object; result.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        result.Transition.StateBefore=from; result.Transition.StateAfter=to; return result;
    }
    [[noreturn]] void fatalExit() const noexcept {
        if(sdlControl && sdlControl->beforeFatalExit) sdlControl->beforeFatalExit();
        ExitProcess(3);
    }
    void retire(ID3D12Fence* fence,std::uint64_t value) {
        const auto deadline=GetTickCount64()+5000; UINT64 observed=0;
        while ((observed=fence->GetCompletedValue())<value && GetTickCount64()<deadline) Sleep(1);
        if (observed==UINT64_MAX || observed<value) fatalExit();
    }
    HRESULT prepareStaging(unsigned index,bool initializationPhase) {
        if(index>=slots.size()) return E_INVALIDARG;
        auto& slot=slots[index];
        if(slot.texture) return S_OK;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC texture{}; texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width=layout.width; texture.Height=layout.height; texture.DepthOrArraySize=texture.MipLevels=1;
        texture.Format=static_cast<DXGI_FORMAT>(driver_bridge::compositorDxgiFormat(layout.format)); texture.SampleDesc.Count=1;
        const auto allocation=device->GetResourceAllocationInfo(0,1,&texture);
        if(!allocation.SizeInBytes || allocation.SizeInBytes==UINT64_MAX || slot.generation==UINT64_MAX || stagingTexturesCreated==UINT_MAX ||
           allocation.SizeInBytes>UINT64_MAX-stagingTextureBytes || allocation.SizeInBytes>UINT64_MAX-liveStagingTextureBytes) return E_INVALIDARG;
        if(budget && !budget("staging-before",allocation.SizeInBytes)) return E_OUTOFMEMORY;
        // Publish ownership only after both admission checks. A failed create
        // or post-create check releases this unused local resource immediately.
        Resource prepared;
        const auto hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&prepared));
        if(FAILED(hr)) return hr;
        if(budget && !budget("staging-after",0)) return E_OUTOFMEMORY;
        slot.texture=std::move(prepared);
        slot.bytes=allocation.SizeInBytes;++slot.generation;slot.reusedAfter=0;
        ++stagingTexturesCreated;stagingTextureBytes+=allocation.SizeInBytes;
        liveStagingTextureBytes+=allocation.SizeInBytes;
        std::cerr<<"{\"nativeCompositorStagingAllocated\":true,\"slot\":"<<index<<",\"bytes\":"<<allocation.SizeInBytes
            <<",\"preparedTextures\":"<<stagingTexturesCreated<<",\"preparedBytes\":"<<stagingTextureBytes
            <<",\"capacity\":3,\"onDemand\":"<<(demandStaging?"true":"false")
            <<",\"generation\":"<<slot.generation<<",\"liveTextureBytes\":"<<liveStagingTextureBytes
            <<",\"phase\":\""<<(initializationPhase?"initialization":"serialized-submit")<<"\"}\n";
        return S_OK;
    }
    HRESULT create() {
        if(budget && !budget("device-before",0)) return E_OUTOFMEMORY;
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        auto hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr=factory->EnumAdapterByLuid(selected,IID_PPV_ARGS(&adapter));
        if (SUCCEEDED(hr)) hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
        D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(hr)) hr=device->CreateCommandQueue(&queue,IID_PPV_ARGS(&inputQueue));
        if (SUCCEEDED(hr)) hr=device->CreateCommandQueue(&queue,IID_PPV_ARGS(&outputQueue));
        if (SUCCEEDED(hr)) hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&inputRetired));
        if (SUCCEEDED(hr)) hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&outputRetired));
        if (SUCCEEDED(hr)) hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&auditRetired));
        if (FAILED(hr)) return hr;
        if(budget && !budget("device-after",0)) return E_OUTOFMEMORY;
        for (unsigned index=0;index<slots.size();++index) {
            auto& slot=slots[index];
            // Slot zero supplies the audit footprint. Other full-size textures
            // are created on first use by the serialized native submit path.
            if(!demandStaging || !index) hr=prepareStaging(index,true);
            if (SUCCEEDED(hr)) hr=device->CreateCommandAllocator(queue.Type,IID_PPV_ARGS(&slot.inputAllocator));
            if (SUCCEEDED(hr)) hr=device->CreateCommandAllocator(queue.Type,IID_PPV_ARGS(&slot.outputAllocator));
            if (SUCCEEDED(hr)) hr=device->CreateCommandList(0,queue.Type,slot.inputAllocator.Get(),nullptr,IID_PPV_ARGS(&slot.inputCommands));
            if (SUCCEEDED(hr)) hr=slot.inputCommands->Close();
            if (SUCCEEDED(hr)) hr=device->CreateCommandList(0,queue.Type,slot.outputAllocator.Get(),nullptr,IID_PPV_ARGS(&slot.outputCommands));
            if (SUCCEEDED(hr)) hr=slot.outputCommands->Close();
            if (FAILED(hr)) return hr;
        }
        // Allocate the bounded audit buffer before publishing initialization.
        // No UI-thread allocation can race the next guest allocation request.
        return prepareAudit();
    }
    HRESULT prepareAudit() {
        if (auditReadback) return S_OK;
        const auto texture=slots[0].texture->GetDesc(); UINT rows=0; UINT64 rowBytes=0;
        device->GetCopyableFootprints(&texture,0,1,0,&footprint,&rows,&rowBytes,&auditBytes);
        if (rows!=layout.height || rowBytes!=layout.stride || footprint.Offset || footprint.Footprint.RowPitch<rowBytes ||
            footprint.Footprint.RowPitch>16384 || footprint.Footprint.RowPitch%256 ||
            auditBytes<std::uint64_t(layout.height-1)*footprint.Footprint.RowPitch+layout.stride || auditBytes>64ull*1024*1024) return E_INVALIDARG;
        if(!planAuditTiles(layout.width,layout.height,footprint.Footprint.RowPitch,auditPlan)) return E_INVALIDARG;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=auditPlan.tileBytes; buffer.Height=1; buffer.DepthOrArraySize=buffer.MipLevels=1;
        buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto allocation=device->GetResourceAllocationInfo(0,1,&buffer);
        if(!allocation.SizeInBytes || allocation.SizeInBytes==UINT64_MAX) return E_INVALIDARG;
        if(budget && !budget("readback-before",allocation.SizeInBytes)) return E_OUTOFMEMORY;
        const auto hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&auditReadback));
        if(SUCCEEDED(hr) && budget && !budget("readback-after",0)) { auditReadback.Reset(); return E_OUTOFMEMORY; }
        return hr;
    }
    HRESULT auditBackbuffer(CompositorOutput& window,Slot& slot,Frame frame,std::uint64_t& hash) {
        // The slot remains occupied until all tile copies retire. A separate
        // fence sequence prevents an already completed frame fence from
        // falsely satisfying a later tile copy before allocator reset/Map.
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(auditPlan.fullBytes));
        for(unsigned n=0;n<auditPlan.count;++n) {
            unsigned top=0,rows=0;
            if(!auditPlan.at(n,top,rows) || auditSequence==UINT64_MAX) return E_INVALIDARG;
            auto hr=slot.outputAllocator->Reset();
            if(SUCCEEDED(hr)) hr=slot.outputCommands->Reset(slot.outputAllocator.Get(),nullptr);
            if(FAILED(hr)) return hr;
            auto tile=footprint;tile.Footprint.Height=rows;
            hr=window.copyAuditTile(slot.outputCommands.Get(),auditReadback.Get(),tile,top,rows);
            if(SUCCEEDED(hr)) hr=slot.outputCommands->Close();
            if(FAILED(hr)) return hr;
            ID3D12CommandList* lists[]{slot.outputCommands.Get()};outputQueue->ExecuteCommandLists(1,lists);
            const auto target=++auditSequence;
            if(FAILED(outputQueue->Signal(auditRetired.Get(),target))) fatalExit();
            retire(auditRetired.Get(),target);
            void* mapped=nullptr;
            const D3D12_RANGE range{0,static_cast<SIZE_T>(std::uint64_t(rows-1)*auditPlan.pitch+layout.stride)};
            hr=auditReadback->Map(0,&range,&mapped);if(FAILED(hr)) return hr;
            for(unsigned y=0;y<rows;++y)
                std::memcpy(pixels.data()+std::uint64_t(top+y)*auditPlan.pitch,
                    static_cast<const std::uint8_t*>(mapped)+std::uint64_t(y)*auditPlan.pitch,layout.stride);
            const D3D12_RANGE empty{};auditReadback->Unmap(0,&empty);
        }
        return driver_capture::writeCompositor(frame.audit,layout.width,layout.height,layout.format,pixels.data(),auditPlan.pitch,hash);
    }
    HRESULT draw(CompositorOutput& window,Frame frame) {
        auto& slot=slots[frame.slot];
        // Input acknowledges retirement before publishing a ready frame. Its
        // wait on outputRetired also proves the previous output allocator is idle.
        const auto inputValue=inputRetired->GetCompletedValue();
        if (inputValue==UINT64_MAX || inputValue<frame.sequence) return E_FAIL;
        auto hr=slot.outputAllocator->Reset();
        if (SUCCEEDED(hr)) hr=slot.outputCommands->Reset(slot.outputAllocator.Get(),nullptr);
        if (SUCCEEDED(hr) && frame.audit) hr=prepareAudit();
        if (FAILED(hr)) return hr;
        auto before=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot.outputCommands->ResourceBarrier(1,&before);
        hr=window.copy(slot.outputCommands.Get(),slot.texture.Get(),nullptr,footprint);
        if (FAILED(hr)) return hr;
        auto after=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        slot.outputCommands->ResourceBarrier(1,&after);
        hr=slot.outputCommands->Close(); if (FAILED(hr)) return hr;
        ID3D12CommandList* lists[]{slot.outputCommands.Get()}; outputQueue->ExecuteCommandLists(1,lists);
        hr=outputQueue->Signal(outputRetired.Get(),frame.sequence); if (FAILED(hr)) fatalExit();
        outputSubmitted=frame.sequence;
        if (frame.audit) {
            retire(outputRetired.Get(),frame.sequence);
            std::uint64_t hash=0;
            hr=auditBackbuffer(window,slot,frame,hash);
            if (FAILED(hr)) return hr;
            ++completedAudits;
            std::cerr<<"{\"nativeCompositorRingAudited\":true,\"frame\":"<<frame.sequence<<",\"auditRound\":"<<frame.audit
                <<",\"fnv1a\":"<<hash<<",\"width\":"<<layout.width<<",\"height\":"<<layout.height
                <<",\"fourcc\":"<<layout.format<<",\"copyFenceObserved\":"<<outputRetired->GetCompletedValue()
                <<",\"readbackTiles\":"<<auditPlan.count<<",\"readbackTileBytes\":"<<auditPlan.tileBytes
                <<",\"auditFenceObserved\":"<<auditRetired->GetCompletedValue()<<",\"backbufferSource\":true}\n";
        }
        {
            std::lock_guard<std::mutex> lock(guard);
            slot.reusedAfter=frame.sequence; slot.occupied=false;
        }
        hr=window.presentQueued(outputQueue.Get(),frame.sequence,frame.audit!=0);
        if (SUCCEEDED(hr)) {
            presented=frame.sequence;
            std::cerr<<"{\"nativeCompositorRingFrame\":true,\"frame\":"<<frame.sequence<<",\"slot\":"<<frame.slot
                <<",\"cpuReadback\":"<<(frame.audit?"true":"false")<<",\"cpuUpload\":false,\"displayVerified\":false}\n";
        }
        return hr;
    }
    void run() noexcept {
        try {
            auto status=create();
            CompositorOutput window(sdlControl);
            struct Drain {
                CompositorPresenter& owner; CompositorOutput& window;
                ~Drain() { window.drainQueued(); if (owner.outputSubmitted) owner.retire(owner.outputRetired.Get(),owner.outputSubmitted); }
            } drain{*this,window};
            if (SUCCEEDED(status)) {
                Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
                status=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
                if (SUCCEEDED(status)) status=window.initialize(factory.Get(),device.Get(),outputQueue.Get(),layout.width,layout.height,
                    static_cast<DXGI_FORMAT>(driver_bridge::compositorDxgiFormat(layout.format)),true,budget,windowInput,inputDiagnostic);
            }
            // Publish the initialization receipt before submit() can allocate
            // another slot on its own thread or publish the first frame.
            if(SUCCEEDED(status)) std::cerr<<"{\"nativeCompositorRingOpened\":true,\"slots\":3,\"sourceCacheLimit\":3,\"uiThreadOwned\":true,\"ordinaryCpuReadback\":false"
                <<",\"onDemandStaging\":"<<(demandStaging?"true":"false")<<",\"initialStagingTextures\":"<<stagingTexturesCreated<<"}\n";
            { std::lock_guard<std::mutex> lock(guard); initialization=status; initialized=true; }
            ready.notify_one();
            if (FAILED(status)) { failure=status; return; }
            for (;;) {
                if (!window.serviceMessages()) { failure=HRESULT_FROM_WIN32(ERROR_CANCELLED); break; }
                Frame frame{}; bool have=false;
                { std::lock_guard<std::mutex> lock(guard);
                  if (!frames.empty()) { frame=frames.front(); frames.pop_front(); have=true; }
                  else if (stopping) break; }
                if (have) {
                    status=draw(window,frame); if (FAILED(status)) { failure=status; break; }
                    if(frame.final) {
                        window.drainQueued();
                        { std::lock_guard<std::mutex> lock(guard);finalized=true;stopping=true; }
                        ready.notify_all();
                        std::cerr<<"{\"nativeCompositorEndRetired\":true,\"frame\":"<<frame.sequence
                            <<",\"beforeFinalGuestReply\":true,\"beforeOwnedVmStop\":true}\n";
                        break;
                    }
                    if(inputDiagnostic && frame.sequence==1) {
                        if(!windowInput || !driver_input::deliveryDiagnostic(*windowInput)) { failure=E_FAIL;break; }
                        std::cerr<<"{\"nativeInputDeliveryDiagnosticQueued\":true,\"afterGpuFrame\":1,\"syntheticAtChannel\":true,\"windowsMessagesVerified\":false}\n";
                    }
                    continue;
                }
                const auto wake=MsgWaitForMultipleObjectsEx(1,&work,1000,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if (wake!=WAIT_OBJECT_0 && wake!=WAIT_OBJECT_0+1 && wake!=WAIT_TIMEOUT) { failure=E_FAIL; break; }
            }
            ready.notify_all();
            window.drainQueued();
            if (outputSubmitted) retire(outputRetired.Get(),outputSubmitted);
            if (completedAudits && SUCCEEDED(failure.load())) {
                const auto until=GetTickCount64()+1000;
                do { if (!window.serviceMessages()) break; Sleep(1); } while (GetTickCount64()<until);
                window.reportQueuedDisplay();
            }
        } catch (...) {
            failure=E_FAIL;
            { std::lock_guard<std::mutex> lock(guard); if (!initialized) { initialization=E_FAIL; initialized=true; } }
            ready.notify_one();
        }
    }
public:
    HRESULT reclaimStagingForResidency(std::uint64_t target,std::uint64_t& released) {
        released=0;
        if(!target || !demandStaging || !device || !initialized || FAILED(initialization)) return S_OK;
        std::lock_guard<std::mutex> lock(guard);
        if(stopping || FAILED(failure.load())) return S_OK;
        const auto input=inputRetired->GetCompletedValue(),output=outputRetired->GetCompletedValue();
        std::array<StagingReclaimState,3> snapshot{};
        for(unsigned index=0;index<slots.size();++index) {
            const auto& slot=slots[index];snapshot[index]={slot.texture.Get()!=nullptr,slot.occupied,slot.bytes,slot.reusedAfter};
        }
        const auto result=reclaimIdleStaging(snapshot,input,output,target,[&](unsigned index,std::uint64_t bytes) {
            auto& slot=slots[index];
            // Clear recorded references only after both actual copy fences
            // retire, with this slot excluded from all queued/UI work.
            HRESULT hr=slot.inputAllocator->Reset();
            if(SUCCEEDED(hr)) hr=slot.inputCommands->Reset(slot.inputAllocator.Get(),nullptr);
            if(SUCCEEDED(hr)) hr=slot.inputCommands->Close();
            if(SUCCEEDED(hr)) hr=slot.outputAllocator->Reset();
            if(SUCCEEDED(hr)) hr=slot.outputCommands->Reset(slot.outputAllocator.Get(),nullptr);
            if(SUCCEEDED(hr)) hr=slot.outputCommands->Close();
            if(FAILED(hr) || bytes>liveStagingTextureBytes || stagingTexturesReleased==UINT_MAX || stagingTexturesTrimmed==UINT_MAX) return false;
            slot.texture.Reset();slot.bytes=0;
            liveStagingTextureBytes-=bytes;++stagingTexturesReleased;++stagingTexturesTrimmed;
            std::cerr<<"{\"nativeCompositorStagingTrimmed\":true,\"slot\":"<<index<<",\"bytes\":"<<bytes
                <<",\"generation\":"<<slot.generation<<",\"reusedAfter\":"<<slot.reusedAfter
                <<",\"inputCopyFenceObserved\":"<<input<<",\"outputCopyFenceObserved\":"<<output
                <<",\"slotOccupied\":false,\"capacity\":3,\"liveTextureBytes\":"<<liveStagingTextureBytes
                <<",\"serializedResidency\":true,\"commandsReset\":true}\n";
            return true;
        });
        released=result.bytes;
        if(result.failed) { failure=E_FAIL;return E_FAIL; }
        return S_OK;
    }
    CompositorPresenter()=default;
    ~CompositorPresenter() { stop(); }
    HRESULT initialize(LUID luid,driver_bridge::CompositorCaptureDesc desc,GpuUsageGuard usage = {},bool demand = false,
                       std::shared_ptr<driver_input::Channel> input = {},bool diagnostic = false,
                       std::shared_ptr<driver_sdl::Control> graphics = {}) {
        if (ui.joinable() || !driver_bridge::validCompositorFrame(desc)) return E_INVALIDARG;
        selected=luid; layout=desc; layout.flags=0; budget=std::move(usage); demandStaging=demand;windowInput=std::move(input);
        inputDiagnostic=diagnostic;sdlControl=std::move(graphics);
        if(inputDiagnostic && !windowInput) return E_INVALIDARG;
        work=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!work) return HRESULT_FROM_WIN32(GetLastError());
        inputEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!inputEvent) { const auto error=GetLastError(); if (!CloseHandle(work)) fatalExit(); work=nullptr; return HRESULT_FROM_WIN32(error); }
        try { ui=std::thread([this]{run();}); }
        catch (...) { failure=E_OUTOFMEMORY; return E_OUTOFMEMORY; }
        std::unique_lock<std::mutex> lock(guard);
        if (!ready.wait_for(lock,std::chrono::seconds(10),[this]{return initialized;})) fatalExit();
        return initialization;
    }
    HRESULT submit(std::uint32_t resourceId,HANDLE resource,HANDLE sync,driver_bridge::CompositorCaptureDesc desc,std::uint64_t& value) {
        value=0;
        if (!resourceId || !resource || resource==INVALID_HANDLE_VALUE || !sync || sync==INVALID_HANDLE_VALUE ||
            !driver_bridge::validCompositorFrame(desc) || desc.width!=layout.width || desc.height!=layout.height || desc.format!=layout.format ||
            !initialized || FAILED(initialization) || submitted==UINT64_MAX-1) return E_INVALIDARG;
        if (FAILED(failure.load())) return failure.load();
        if(budget && !budget("frame-before",0)) return E_OUTOFMEMORY;
        unsigned index=0;
        { std::lock_guard<std::mutex> lock(guard);
          if (stopping || ending) return E_FAIL;
          while (index<slots.size() && slots[index].occupied) ++index;
          if (index==slots.size()) return HRESULT_FROM_WIN32(ERROR_RETRY);
          slots[index].occupied=true; }
        auto& slot=slots[index]; bool inputSubmitted=false, inputCompleted=false, published=false;
        struct Rollback { CompositorPresenter& owner; Slot& slot; std::mutex& guard; std::atomic<HRESULT>& failure; bool& submitted; bool& completed; bool& published;
            ~Rollback() {
                if (submitted && !completed) owner.fatalExit();
                if (submitted && !published) failure=E_FAIL;
                std::lock_guard<std::mutex> lock(guard); if (!published) slot.occupied=false;
            }
        } rollback{*this,slot,guard,failure,inputSubmitted,inputCompleted,published};
        // Native calls are serialized; do not allocate a new texture on the UI
        // thread while a guest residency operation is admitted elsewhere.
        auto staging=prepareStaging(index,false);
        if(FAILED(staging)) return staging;
        auto source=sources.find(resourceId);
        HRESULT hr=S_OK;
        if (source==sources.end()) {
            if (sources.size()>=3) return E_OUTOFMEMORY;
            if(budget && !budget("import-before",0)) return E_OUTOFMEMORY;
            Resource imported; hr=device->OpenSharedHandle(resource,IID_PPV_ARGS(&imported)); if (FAILED(hr)) return hr;
            if(budget && !budget("import-after",0)) return E_OUTOFMEMORY;
            const auto actual=imported->GetDesc();
            if (actual.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.Width!=layout.width || actual.Height!=layout.height ||
                actual.Format!=static_cast<DXGI_FORMAT>(driver_bridge::compositorDxgiFormat(layout.format)) || actual.DepthOrArraySize!=1 ||
                actual.MipLevels!=1 || actual.SampleDesc.Count!=1 || actual.SampleDesc.Quality || actual.Flags!=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) return E_INVALIDARG;
            source=sources.emplace(resourceId,std::move(imported)).first;
            peakSources=(std::max)(peakSources,static_cast<unsigned>(sources.size()));
        }
        Microsoft::WRL::ComPtr<ID3D12Fence> producer;
        hr=device->OpenSharedHandle(sync,IID_PPV_ARGS(&producer)); if (FAILED(hr)) return hr;
        // The imported source is already allocated by the guest; do not charge
        // its full size twice. Observe the process usage after opening aliases.
        if(budget && !budget("frame-admitted",0)) return E_OUTOFMEMORY;
        hr=slot.inputAllocator->Reset(); if (SUCCEEDED(hr)) hr=slot.inputCommands->Reset(slot.inputAllocator.Get(),nullptr);
        if (FAILED(hr)) return hr;
        const auto a=transition(source->second.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto b=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        slot.inputCommands->ResourceBarrier(1,&a); slot.inputCommands->ResourceBarrier(1,&b);
        slot.inputCommands->CopyResource(slot.texture.Get(),source->second.Get());
        const auto c=transition(source->second.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        const auto d=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        slot.inputCommands->ResourceBarrier(1,&c); slot.inputCommands->ResourceBarrier(1,&d);
        hr=slot.inputCommands->Close(); if (FAILED(hr)) return hr;
        unsigned audit=0;
        if (desc.flags & driver_bridge::CompositorFrameAudit) { if (auditAttempts>=2) return E_INVALIDARG; audit=++auditAttempts; }
        hr=inputQueue->Wait(producer.Get(),desc.fence); if (FAILED(hr)) return hr;
        inputSubmitted=true;
        if (slot.reusedAfter) { hr=inputQueue->Wait(outputRetired.Get(),slot.reusedAfter); if (FAILED(hr)) fatalExit(); }
        ID3D12CommandList* lists[]{slot.inputCommands.Get()}; inputQueue->ExecuteCommandLists(1,lists);
        const auto frame=submitted+1;
        hr=inputQueue->Signal(inputRetired.Get(),frame); if (FAILED(hr)) fatalExit();
        if (inputRetired->GetCompletedValue()<frame) {
            hr=inputRetired->SetEventOnCompletion(frame,inputEvent);
            if (FAILED(hr) || WaitForSingleObject(inputEvent,5000)!=WAIT_OBJECT_0) fatalExit();
        }
        const auto copied=inputRetired->GetCompletedValue();
        if (copied==UINT64_MAX || copied<frame) fatalExit();
        inputCompleted=true;
        if (producer->GetCompletedValue()<desc.fence || producer->GetCompletedValue()==UINT64_MAX) return E_FAIL;
        std::cerr<<"{\"nativeCompositorRingInput\":true,\"frame\":"<<frame<<",\"slot\":"<<index<<",\"sourceTicket\":"<<resourceId
            <<",\"producerFenceTarget\":"<<desc.fence<<",\"producerFenceObserved\":"<<producer->GetCompletedValue()
            <<",\"inputCopyFenceObserved\":"<<inputRetired->GetCompletedValue()<<",\"sourceReturnedToCommon\":true,\"sourceCopyRetired\":true,\"cpuReadback\":false,\"cpuUpload\":false}\n";
        const bool final=(desc.flags&driver_bridge::CompositorFrameFinal)!=0;
        { std::lock_guard<std::mutex> lock(guard); frames.push_back({index,audit,frame,final});ending=final;
          peakQueued=(std::max)(peakQueued,static_cast<unsigned>(frames.size())); }
        submitted=frame; value=frame; published=true;
        if (!SetEvent(work)) fatalExit();
        if(final) {
            std::unique_lock<std::mutex> lock(guard);
            if(!ready.wait_for(lock,std::chrono::seconds(15),[this]{return finalized || FAILED(failure.load());}) ||
               !finalized || FAILED(failure.load())) {
                value=0;failure=E_FAIL;return E_FAIL;
            }
        }
        return S_OK;
    }
    void releaseSource(std::uint32_t resource) { sources.erase(resource); }
    ThreadCompletion pollStop() {
        if (ui.joinable()) {
            { std::lock_guard<std::mutex> lock(guard); stopping=true; }
            if (!SetEvent(work)) return ThreadCompletion::Failed;
        }
        return queryOwnedThreadCompletion(ui);
    }
    bool stop() {
        if (!ui.joinable()) {
            if (work && !CloseHandle(work)) fatalExit();
            if (inputEvent && !CloseHandle(inputEvent)) fatalExit();
            work=inputEvent=nullptr;
            return SUCCEEDED(failure.load());
        }
        { std::lock_guard<std::mutex> lock(guard); stopping=true; }
        if (!SetEvent(work)) fatalExit();
        ui.join();
        const bool clean=SUCCEEDED(failure.load()) && presented==submitted && frames.empty() &&
            std::none_of(slots.begin(),slots.end(),[](const Slot& slot){return slot.occupied;});
        sources.clear();
        for (auto& slot:slots) {
            slot.inputCommands.Reset(); slot.outputCommands.Reset(); slot.inputAllocator.Reset(); slot.outputAllocator.Reset();
            if(slot.texture) { slot.texture.Reset();liveStagingTextureBytes-=slot.bytes;slot.bytes=0;++stagingTexturesReleased; }
        }
        auditReadback.Reset();auditRetired.Reset(); inputQueue.Reset(); outputQueue.Reset(); inputRetired.Reset(); outputRetired.Reset(); device.Reset();
        const bool closed=CloseHandle(work)!=FALSE; work=nullptr;
        const bool inputClosed=CloseHandle(inputEvent)!=FALSE; inputEvent=nullptr;
        std::cerr<<"{\"nativeCompositorRingStopped\":"<<(clean&&closed?"true":"false")<<",\"submitted\":"<<submitted<<",\"presented\":"<<presented
            <<",\"completedAudits\":"<<completedAudits<<",\"peakQueued\":"<<peakQueued<<",\"peakSources\":"<<peakSources
            <<",\"onDemandStaging\":"<<(demandStaging?"true":"false")<<",\"stagingTexturesCreated\":"<<stagingTexturesCreated
            <<",\"stagingTexturesReleased\":"<<stagingTexturesReleased<<",\"stagingTextureBytes\":"<<stagingTextureBytes
            <<",\"stagingTexturesTrimmed\":"<<stagingTexturesTrimmed<<",\"liveStagingTextureBytes\":"<<liveStagingTextureBytes
            <<",\"workHandleClosed\":"<<(closed?"true":"false")<<",\"inputEventClosed\":"<<(inputClosed?"true":"false")
            <<",\"uiThreadJoined\":true,\"liveGpuResources\":0,\"displayVerified\":false}\n";
        if (!closed || !inputClosed) fatalExit();
        return clean;
    }
};
}
