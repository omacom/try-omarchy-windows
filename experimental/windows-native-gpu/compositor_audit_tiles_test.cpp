// SPDX-License-Identifier: MIT
#include "compositor_audit_tiles.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
static void require(bool value) { if(!value) throw std::runtime_error("Diagnostic tile extent or stitching control"); }
int main() {
    using namespace driver_shared;
    for(const auto& dimensions:{std::pair<unsigned,unsigned>{1,1},{130,73},{640,360},{3840,2160},{4096,4096}}) {
        const auto width=dimensions.first,height=dimensions.second,pitch=(width*4+255)&~255u;
        AuditTilePlan plan;require(planAuditTiles(width,height,pitch,plan));
        require(plan.fullBytes==std::uint64_t(height)*pitch && plan.tileBytes<=4u*1024*1024);
        std::vector<unsigned char> captured(std::size_t(width)*height*4+32,0x7d);
        unsigned expectedTop=0;
        for(unsigned n=0;n<plan.count;++n) {
            unsigned top=0,rows=0;require(plan.at(n,top,rows) && top==expectedTop && rows<=256 && rows<=height-top);
            std::vector<unsigned char> mapped(std::size_t(rows)*pitch+32,0xcd);
            for(unsigned y=0;y<rows;++y) for(unsigned x=0;x<width*4;++x)
                mapped[16+std::size_t(y)*pitch+x]=static_cast<unsigned char>(((top+y)*17+x*31)%251);
            for(unsigned y=0;y<rows;++y)
                std::memcpy(captured.data()+16+std::size_t(top+y)*width*4,mapped.data()+16+std::size_t(y)*pitch,width*4);
            require(std::all_of(mapped.begin(),mapped.begin()+16,[](auto v){return v==0xcd;}) &&
                std::all_of(mapped.end()-16,mapped.end(),[](auto v){return v==0xcd;}));
            expectedTop+=rows;
        }
        require(expectedTop==height);
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width*4;++x)
            require(captured[16+std::size_t(y)*width*4+x]==static_cast<unsigned char>((y*17+x*31)%251));
        require(std::all_of(captured.begin(),captured.begin()+16,[](auto v){return v==0x7d;}) &&
            std::all_of(captured.end()-16,captured.end(),[](auto v){return v==0x7d;}));
        unsigned top=1,rows=1;require(!plan.at(plan.count,top,rows) && !top && !rows);
        require(!plan.at(UINT32_MAX,top,rows) && !top && !rows);
        if(width==3840) require(plan.count==9 && plan.tileBytes==3932160 && plan.at(8,top,rows) && top==2048 && rows==112);
    }
    for(const auto& invalid:{std::pair<unsigned,unsigned>{0,1},{1,0},{4097,1},{1,4097},{UINT32_MAX,UINT32_MAX}}) {
        AuditTilePlan plan;require(!planAuditTiles(invalid.first,invalid.second,16384,plan) && !plan.count);
    }
    for(const auto pitch:{0u,255u,257u,15359u,16385u,UINT32_MAX}) {
        AuditTilePlan plan;require(!planAuditTiles(3840,2160,pitch,plan) && !plan.tileBytes);
    }
    std::cout<<"AUDIT_TILE_CONTROL full4kExtent=true nineTiles=true oddStrideAndLastPartialRows=true destinationGuards=true maxGpuReadbackBytes=4194304 gpu=false\n";
}
