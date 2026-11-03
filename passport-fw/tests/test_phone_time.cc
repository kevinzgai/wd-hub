#include <cassert>
#include <initializer_list>
#include <cstdio>
#include "phone_time.h"
int main() {
    for (int tz : {-720, 0, 345, 480, 840}) {
        char text[128]; snprintf(text,sizeof(text),"{\"epoch\":1790000000,\"tz_minutes\":%d}",tz);
        auto *j=cJSON_Parse(text);int64_t epoch;int minutes;
        assert(wifi_phone_time_parse(j,&epoch,&minutes) && epoch==1790000000 && minutes==tz);cJSON_Delete(j);
    }
    for (const char *text : {"{}", "{\"epoch\":1,\"tz_minutes\":0}",
         "{\"epoch\":1790000000,\"tz_minutes\":841}", "{\"epoch\":1790000000,\"tz_minutes\":-735}",
         "{\"epoch\":1790000000,\"tz_minutes\":1}", "{\"epoch\":1790000000.5,\"tz_minutes\":0}",
         "{\"epoch\":\"1790000000\",\"tz_minutes\":0}", "{\"epoch\":1e300,\"tz_minutes\":0}"}) {
        auto *j=cJSON_Parse(text);int64_t epoch;int minutes;
        assert(!wifi_phone_time_parse(j,&epoch,&minutes));cJSON_Delete(j);
    }
    puts("Phone time: epoch/type/range and 15-minute timezone validation PASS");
}
