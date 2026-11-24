// SPDX-License-Identifier: MIT
#include "fair_rpc_mutex.h"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
static void require(bool ok) {if(!ok) throw std::runtime_error("Fair RPC scheduling control failed");}
static void ordered(std::uint64_t first) {
    driver_bridge::FairRpcMutex mutex(first);std::vector<unsigned> order;
    std::array<std::thread,3> workers;
    mutex.lock();
    for(unsigned i=0;i<workers.size();++i) {
        workers[i]=std::thread([&,i]{std::lock_guard<driver_bridge::FairRpcMutex> guard(mutex);order.push_back(i);});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(mutex.pending()!=i+2 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        require(mutex.pending()==i+2);
    }
    mutex.unlock();for(auto& worker:workers) worker.join();
    require(order==std::vector<unsigned>({0,1,2}) && !mutex.pending());
}
int main() {
    ordered(0);ordered(UINT64_MAX-1);
    driver_bridge::FairRpcMutex mutex;unsigned total=0;
    std::array<unsigned,4> completed{};std::array<std::thread,4> workers;
    for(unsigned i=0;i<workers.size();++i) workers[i]=std::thread([&,i]{
        for(unsigned n=0;n<1000;++n) {std::lock_guard<driver_bridge::FairRpcMutex> guard(mutex);++total;++completed[i];}
    });
    for(auto& worker:workers) worker.join();
    require(total==4000 && !mutex.pending());for(const auto count:completed) require(count==1000);
    std::cout<<"FAIR_RPC_CONTROL fifo=true singleOwner=true allCallersProgress=true sequenceWrap=true calls=4000 gpu=false\n";
}
