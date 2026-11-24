// SPDX-License-Identifier: MIT
#include "deferred_residency.h"
#include <stdexcept>
#include <iostream>
static void require(bool value) { if(!value) throw std::runtime_error("residency ownership regression"); }
int main() {
    using driver_residency::References;
    References refs;
    require(!refs.defer() && !refs.reuse() && !refs.release(false,true));
    require(refs.admit() && refs.native()==1);
    // A persistent CPU mapping can survive arbitrary guest eviction/reuse
    // cycles without dropping its real reference or making another native one.
    for(unsigned n=0;n<100000;++n) {
        require(refs.defer() && refs.guest==0 && refs.retained==1 && refs.native()==1);
        require(!refs.release(true,true) && !refs.release(false,false));
        require(refs.reuse() && refs.guest==1 && refs.retained==0 && refs.native()==1);
    }
    require(refs.admit() && refs.defer() && refs.defer());
    require(refs.guest==0 && refs.retained==2 && !refs.defer());
    require(refs.release(false,true) && refs.native()==1);
    require(refs.reuse() && refs.guest==1 && refs.native()==1);
    require(refs.defer() && refs.release(false,true) && refs.native()==0);
    References maximum{UINT32_MAX,0,true};
    require(!maximum.admit() && maximum.defer() && maximum.native()==UINT32_MAX);
    require(!maximum.admit() && maximum.reuse() && maximum.guest==UINT32_MAX);
    References overflow{UINT32_MAX,1,true};
    require(!overflow.admit() && !overflow.defer() && !overflow.reuse() && !overflow.release(false,true));
    References uncertain{1,2,true};uncertain.uncertain();
    require(!uncertain.admit() && !uncertain.defer() && !uncertain.reuse() && !uncertain.release(false,true));
    require(uncertain.native()==3);
    std::cout<<"PASS: mapped reference retention, retired-only reclamation, reuse without native growth, overflow, uncertain ownership\n";
}
