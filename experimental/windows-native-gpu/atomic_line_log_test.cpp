// SPDX-License-Identifier: MIT
#include "atomic_line_log.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
int main() {
    std::ostringstream output;
    {
        AtomicLineLog log(output);
        std::array<std::thread,8> writers;
        for (unsigned id=0;id<writers.size();++id) writers[id]=std::thread([id,&output] {
            for (unsigned sequence=0;sequence<500;++sequence) {
                output<<"{\"thread\":"<<id<<",\"frame\":"<<sequence<<",\"complete\":true}"<<std::endl;
            }
        });
        for (auto& writer:writers) writer.join();
    }
    std::set<std::string> expected;
    for (unsigned id=0;id<8;++id) for (unsigned sequence=0;sequence<500;++sequence)
        expected.insert("{\"thread\":"+std::to_string(id)+",\"frame\":"+std::to_string(sequence)+",\"complete\":true}");
    std::istringstream input(output.str()); std::string line;
    while (std::getline(input,line)) if (expected.erase(line)!=1) return EXIT_FAILURE;
    if (!expected.empty()) return EXIT_FAILURE;
    std::cout<<"PASS: 4000 concurrent multi-insertion receipts remain complete, unique lines\n";
}
