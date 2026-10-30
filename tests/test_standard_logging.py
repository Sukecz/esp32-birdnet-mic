"""Fixed RAM budget and append-only SD journal behavior on real temporary files."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class StandardLoggingTests(unittest.TestCase):
 def run_cpp(self,code):
  with tempfile.TemporaryDirectory() as folder:
   p=Path(folder);(p/'test.cpp').write_text(code)
   subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(ROOT/'esp32-birdnet-mic'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],cwd=p,check=True,timeout=15)
 def test_bounded_ram_preserves_complete_recent_lines_without_allocation(self):
  self.run_cpp(r'''
#include "BoundedLog.h"
#include <cassert>
#include <deque>
#include <string>
int main(){
 BoundedLog<97> log;std::deque<std::string> expected;size_t total=0;
 for(int i=0;i<10000;++i){
  const auto line=std::to_string(i)+std::string(i%43,'x');
  log.append(line.c_str(),line.size());expected.push_back(line);total+=line.size()+1;
  while(total>97){total-=expected.front().size()+1;expected.pop_front();}
  std::string actual;for(size_t j=0;j<log.size();++j)actual+=log.at(j);
  std::string model;for(const auto& item:expected)model+=item+'\n';
  assert(actual==model&&log.size()<=97);
 }
 char line[100];while(!expected.empty()){assert(log.pop(line,sizeof(line)));assert(expected.front()==line);expected.pop_front();}
 assert(!log.pop(line,sizeof(line)));log.clear();
 log.append("abc\n",4);assert(log.size()==4);assert(log.pop(line,sizeof(line))&&std::string(line)=="abc");
 log.append(std::string(200,'z').c_str(),200);assert(log.size()==97);assert(log.pop(line,sizeof(line))&&strlen(line)==96);
}
''')
 def test_journal_appends_reopens_rotates_and_preserves_other_files(self):
  self.run_cpp(r'''
#include "SdJournal.h"
#include <cassert>
#include <fstream>
#include <filesystem>
#include <string>
std::string read(const char* file){std::ifstream f(file);return {std::istreambuf_iterator<char>(f),{}};}
int main(){
 {std::ofstream("audio.wav")<<"protected audio";}
 SdJournal journal;assert(journal.begin("recording.log",64));assert(journal.append("first boot"));assert(journal.flush());assert(journal.close());
 assert(read("recording.log")=="first boot\n");
 assert(journal.begin("recording.log",64));assert(journal.append("next boot"));assert(journal.close());assert(read("recording.log")=="first boot\nnext boot\n");
 assert(journal.begin("recording.log",64));for(int i=0;i<1000;++i)assert(journal.append(("line-"+std::to_string(i)).c_str()));assert(journal.close());
 assert(std::filesystem::file_size("recording.log")<=64&&std::filesystem::file_size("recording.log.1")<=64);
 assert(read("recording.log").find("line-999\n")!=std::string::npos);assert(read("audio.wav")=="protected audio");
 std::filesystem::create_directory("blocked.log");assert(!journal.begin("blocked.log",64));assert(journal.errorCode()==EISDIR);assert(read("audio.wav")=="protected audio");
 assert(journal.begin("large.log",16));assert(!journal.append(std::string(17,'a').c_str()));assert(journal.errorCode()==EFBIG);
}
''')
if __name__=='__main__':unittest.main()
