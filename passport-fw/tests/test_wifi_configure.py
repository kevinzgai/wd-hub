"""Execute the production save boundary with successful and failed connections."""
from pathlib import Path
import subprocess
import tempfile
source = Path('components/esp-wifi-connect/wifi_manager.cc').read_text()
body = source[source.index('bool WifiManager::ConfigureNetwork('):source.index('// ==================== Power')]
prelude = r'''
#include <cassert>
#include <mutex>
#include <memory>
#include <string>
struct WifiConfigurationAp {
 bool success=false; int saves=0;
 bool ConnectToWifi(const std::string&,const std::string&,bool){return success;}
 void Save(const std::string&,const std::string&){++saves;}
 WifiConfigurationAp *get(){return this;}
};
struct WifiManager {
 std::mutex mutex_; bool config_mode_active_=true; std::unique_ptr<WifiConfigurationAp> config_ap_; int stops=0;
 void StopConfigAp(){++stops;}
 void AllowNetwork(const std::string&){}
 bool ConfigureNetwork(const std::string&,const std::string&,bool);
};
'''
main = r'''
int main(){
 WifiManager wifi; wifi.config_ap_=std::make_unique<WifiConfigurationAp>();auto &ap=*wifi.config_ap_;
 for(int i=0;i<10;++i)assert(!wifi.ConfigureNetwork("test","bad",true));
 assert(ap.saves==0 && wifi.stops==0);
 ap.success=true;assert(wifi.ConfigureNetwork("test","password",true));
 assert(ap.saves==1 && wifi.stops==0);
 assert(wifi.ConfigureNetwork("open","",false));assert(ap.saves==2 && wifi.stops==1);
 wifi.config_mode_active_=false;assert(!wifi.ConfigureNetwork("test","password",true));assert(ap.saves==2);
}
'''
with tempfile.TemporaryDirectory() as temp:
    path=Path(temp)/'test.cc'; exe=Path(temp)/'test'
    path.write_text(prelude+body+main)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-g',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Wi-Fi configure: success saves once, failure/closed page never save PASS')
