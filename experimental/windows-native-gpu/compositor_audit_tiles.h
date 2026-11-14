// SPDX-License-Identifier: MIT
// Bound diagnostic GPU readback without reducing the captured image extent.
#pragma once
#include <cstdint>
namespace driver_shared {
constexpr unsigned AuditTileRows=256;
struct AuditTilePlan {
    unsigned height=0,pitch=0,count=0;
    std::uint64_t fullBytes=0,tileBytes=0;
    bool at(unsigned index,unsigned& top,unsigned& rows) const {
        top=rows=0;
        if(!count || index>=count) return false;
        top=index*AuditTileRows;
        rows=height-top<AuditTileRows ? height-top : AuditTileRows;
        return rows!=0;
    }
};
inline bool planAuditTiles(unsigned width,unsigned height,unsigned pitch,AuditTilePlan& out) {
    out={};
    if(!width || width>4096 || !height || height>4096 || pitch<width*4 || pitch>16384 || pitch%256) return false;
    out={height,pitch,(height+AuditTileRows-1)/AuditTileRows,std::uint64_t(height)*pitch,
        std::uint64_t(height<AuditTileRows ? height : AuditTileRows)*pitch};
    return true;
}
}
