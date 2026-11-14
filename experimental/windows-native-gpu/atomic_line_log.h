// SPDX-License-Identifier: MIT
// Keep receipts from the ingress and UI threads as whole lines under C++17.
#pragma once
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>
class AtomicLineLog final : private std::streambuf {
    std::ostream& stream;
    std::streambuf* original;
    std::mutex guard;
    inline static thread_local std::string pending;
    std::streamsize xsputn(const char* data,std::streamsize size) override {
        for (std::streamsize index=0;index<size;++index) {
            pending.push_back(data[index]);
            if (data[index]=='\n') {
                std::lock_guard<std::mutex> lock(guard);
                const auto count=static_cast<std::streamsize>(pending.size());
                if (original->sputn(pending.data(),count)!=count || original->pubsync()!=0) return index;
                pending.clear();
            }
        }
        return size;
    }
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character,traits_type::eof())) return traits_type::not_eof(character);
        const char value=traits_type::to_char_type(character);
        return xsputn(&value,1)==1 ? character : traits_type::eof();
    }
    // cerr is unit-buffered: flushing each insertion must not publish partial JSON.
    int sync() override { return 0; }
public:
    explicit AtomicLineLog(std::ostream& destination) : stream(destination),original(destination.rdbuf(this)) {}
    ~AtomicLineLog() override { stream.rdbuf(original); }
    AtomicLineLog(const AtomicLineLog&)=delete;
    AtomicLineLog& operator=(const AtomicLineLog&)=delete;
};
