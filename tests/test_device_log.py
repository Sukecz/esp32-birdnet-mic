"""Mode budgets, PSRAM fallback and lossless bounded paging of the real UI log."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
STUB=r'''
#pragma once
#include <cstdlib>
#include <cstddef>
#include <unordered_map>
#include <cassert>
constexpr int MALLOC_CAP_8BIT=1,MALLOC_CAP_INTERNAL=2,MALLOC_CAP_SPIRAM=4;
inline size_t externalAvailable=0,internalLive=0,externalLive=0,internalPeak=0;
inline bool failInternal=false,failExternal=false;
inline std::unordered_map<void*,std::pair<size_t,bool>> allocations;
inline size_t heap_caps_get_free_size(int caps){return caps&MALLOC_CAP_SPIRAM?externalAvailable-externalLive:100000-internalLive;}
inline void* heap_caps_malloc(size_t n,int caps){
 bool ext=caps&MALLOC_CAP_SPIRAM;if(ext?failExternal:failInternal)return nullptr;
 void* p=malloc(n);assert(p);allocations[p]={n,ext};if(ext)externalLive+=n;else {internalLive+=n;if(internalLive>internalPeak)internalPeak=internalLive;}return p;
}
inline void heap_caps_free(void* p){if(!p)return;auto i=allocations.find(p);assert(i!=allocations.end());if(i->second.second)externalLive-=i->second.first;else internalLive-=i->second.first;allocations.erase(i);free(p);}
'''
class DeviceLogTests(unittest.TestCase):
 def run_cpp(self,body):
  with tempfile.TemporaryDirectory() as folder:
   p=Path(folder);(p/'esp_heap_caps.h').write_text(STUB);(p/'test.cpp').write_text('#include "DeviceLog.h"\n#include <string>\n#include <vector>\n#include <algorithm>\n'+body)
   subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(p),'-I',str(ROOT/'esp32-birdnet-mic'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True,timeout=15)
 def test_internal_mode_resize_preserves_recent_lines_and_handles_oom(self):
  self.run_cpp(r'''
int main(){
 {DeviceLog log;assert(log.configure(false));assert(log.capacity()==8192&&!log.usesPsram()&&internalLive==8192);
 for(int i=0;i<200;++i){const auto text=std::to_string(i)+std::string(70,'x');log.append(text.c_str(),text.size());}
 const auto end=log.endCursor();std::string before;for(size_t i=0;i<log.size();++i)before+=log.at(i);
 assert(log.configure(true)&&log.capacity()==4096&&internalLive==4096&&internalPeak<=12288);
 std::string after;for(size_t i=0;i<log.size();++i)after+=log.at(i);assert(before.substr(before.size()-after.size())==after&&log.endCursor()==end);
 failInternal=true;assert(!log.configure(false)&&log.capacity()==4096&&internalLive==4096);log.append("still works",11);
 assert(log.configure(true));failInternal=false;assert(log.configure(false)&&internalLive==8192);
 failInternal=true;assert(!log.configure(true)&&log.capacity()==8192&&internalLive==8192);failInternal=false;
 }assert(internalLive==0&&externalLive==0&&allocations.empty());
}
''')
 def test_psram_capacity_and_failed_psram_use_small_internal_fallback(self):
  self.run_cpp(r'''
int main(){
 externalAvailable=1024*1024;
 {DeviceLog log;assert(log.configure(false)&&log.usesPsram()&&log.capacity()==32768);assert(internalLive==0&&externalLive==32768);
 assert(log.configure(true)&&log.capacity()==32768&&externalLive==32768);
 for(int i=0;i<10000;++i){const auto text=std::to_string(i)+std::string(90,'y');log.append(text.c_str(),text.size());}assert(log.size()<=32768&&internalLive==0&&externalLive==32768);}
 failExternal=true;
 {DeviceLog log;assert(log.configure(false)&&!log.usesPsram()&&log.capacity()==8192&&internalLive==8192);assert(log.configure(true)&&internalLive==4096);}
 assert(allocations.empty());
}
''')
 def test_pages_cover_retained_history_and_detect_overwritten_cursors(self):
  self.run_cpp(r'''
int main(){
 externalAvailable=1024*1024;DeviceLog log;assert(log.configure(false));
 for(int i=0;i<1000;++i){const auto text="line-"+std::to_string(i)+std::string(91,'z');log.append(text.c_str(),text.size());}
 std::string all;char chunk[2048];for(size_t i=0;i<log.size();){size_t n=log.copy(i,chunk,1024);all.append(chunk,n);i+=n;}
 assert(all.size()==log.size()&&log.beginCursor()>0);
 std::string backwards;auto p=log.page(chunk,sizeof(chunk));assert(p.end==log.endCursor());
 while(true){assert(p.bytes<=2048);if(p.bytes)assert(chunk[p.bytes-1]=='\n');backwards=std::string(chunk,p.bytes)+backwards;if(p.start==p.first)break;p=log.page(chunk,sizeof(chunk),false,true,p.start);}
 assert(backwards==all);
 std::string forwards;p=log.page(chunk,sizeof(chunk),true,true,log.beginCursor());
 while(true){forwards.append(chunk,p.bytes);if(p.end==p.last)break;assert(p.bytes);p=log.page(chunk,sizeof(chunk),true,true,p.end);}assert(forwards==all);
 auto moved=log.page(chunk,sizeof(chunk),false,true,0);assert(moved.cursorMoved&&moved.bytes==0);
 moved=log.page(chunk,sizeof(chunk),true,true,0);assert(moved.cursorMoved&&moved.start==log.beginCursor()&&moved.bytes);
 moved=log.page(chunk,sizeof(chunk),false,true,UINT64_MAX);assert(moved.cursorMoved&&moved.end==log.endCursor());
 uint64_t value;assert(DeviceLog::parseCursor("18446744073709551615",value)&&value==UINT64_MAX);
 assert(!DeviceLog::parseCursor("18446744073709551616",value));assert(!DeviceLog::parseCursor("-1",value));assert(!DeviceLog::parseCursor("1.0",value));assert(!DeviceLog::parseCursor("",value));
}
''')
if __name__=='__main__':unittest.main()
