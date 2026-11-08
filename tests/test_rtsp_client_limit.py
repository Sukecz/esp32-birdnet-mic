"""Run the actual max_clients setter branch against its session-array limit."""
import pathlib
import re
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class ClientLimitTest(unittest.TestCase):
    def test_all_slots_and_invalid_values(self):
        source=(ROOT/'esp32-birdnet-mic/WebUI.cpp').read_text()
        branch=source.split('else if (key == "max_clients") {',1)[1].split('\n    }',1)[0]
        types=(ROOT/'esp32-birdnet-mic/StreamTypes.h').read_text()
        limit=re.search(r'constexpr\s+\w+\s+MAX_CLIENTS\s*=\s*(\d+)',types)
        self.assertIsNotNone(limit)
        code='''#include <cassert>
#include <cstdint>
constexpr unsigned MAX_CLIENTS='''+limit[1]+''';
unsigned value=0; bool parse=true,handled=false,applied=false;uint8_t maxActiveClients=1;unsigned saves=0;
bool argToUInt(uint32_t& v){v=value;return parse;}void saveAudioSettings(){++saves;}
void set(){'''+branch+'''}
int main(){for(value=1;value<=MAX_CLIENTS;++value){applied=false;set();assert(applied&&handled&&maxActiveClients==value);}assert(saves==MAX_CLIENTS);
for(unsigned bad:{0u,MAX_CLIENTS+1,255u}){value=bad;applied=false;set();assert(!applied&&saves==MAX_CLIENTS);}parse=false;value=1;set();assert(saves==MAX_CLIENTS);}
'''
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'t.cpp').write_text('#include <initializer_list>\n'+code)
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror',str(p/'t.cpp'),'-o',str(p/'t')],check=True)
            subprocess.run([str(p/'t')],check=True)
