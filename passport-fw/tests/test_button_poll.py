"""Run production ADC polling against short taps at every sample phase."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/main.cc').read_text()
start = source.index('static int button_from_mv(')
end = source.index('static void input_task(', start)
polling = source[start:end]
pins = (root / 'components/bsp/include/bsp_pins.h').read_text()
ranges = next(line for line in pins.splitlines() if line.startswith('#define BSP_BTN_MV_TABLE'))
code = ranges + '\n#define BSP_BTN_COUNT 3\n' + r'''
#include <cassert>
#include <cstdint>
#include <vector>
enum app_btn_event_t { BTN_EVT_CLICK, BTN_EVT_PRESS, BTN_EVT_RELEASE, BTN_EVT_LONG, BTN_EVT_LONG_UP };
enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK };
struct KeyMessage { uint8_t button; app_btn_event_t event; uint16_t mv; };
using TickType_t = unsigned;
static constexpr uint8_t kPowerCommand = 0xfe;
static unsigned now, tap_start, tap_end;
static int voltage;
static bool screen_on;
static std::vector<KeyMessage> messages;
struct Done {};
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGI(...) ((void)0)
static unsigned xTaskGetTickCount() { return now; }
static void vTaskDelay(unsigned delay) { now += delay; if (now >= 250) throw Done{}; }
static int bsp_button_read_mv() { return now >= tap_start && now < tap_end ? voltage : 2900; }
[[maybe_unused]] static bool app_registry_screen_is_on() { return screen_on; }
static void key_post(KeyMessage message) { messages.push_back(message); }
''' + polling + r'''
int main() {
    assert(button_from_mv(-1) == -1);
    for (int mv : {0, 5, 10, 149}) assert(button_from_mv(mv) == BSP_BTN_UP);
    for (int mv : {150, 280, 302, 320, 449}) assert(button_from_mv(mv) == BSP_BTN_DOWN);
    for (int mv : {450, 580, 603, 630, 999}) assert(button_from_mv(mv) == BSP_BTN_OK);
    for (int mv : {1000, 2500, 2900, 3300}) assert(button_from_mv(mv) == -1);
    for (bool lit : {false, true}) for (int button = 0; button < 3; ++button) {
        for (unsigned phase = 0; phase < 50; ++phase) {
            now = 0; tap_start = 1 + phase; tap_end = tap_start + 80;
            voltage = button == BSP_BTN_UP ? 5 : button == BSP_BTN_DOWN ? 302 : 603;
            screen_on = lit; messages.clear();
            try { button_task(nullptr); } catch (Done &) {}
            assert(messages.size() == 3);
            assert(messages[0].event == BTN_EVT_PRESS);
            assert(messages[1].event == BTN_EVT_CLICK);
            assert(messages[2].event == BTN_EVT_RELEASE);
            for (const auto &message : messages) assert(message.button == button);
        }
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'button.cpp'
    path.write_text(code)
    executable = str(path.with_suffix(''))
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', executable], check=True)
    subprocess.run([executable], check=True)
print('ADC polling: 80 ms taps for all three keys, screen on/off, 50 sample phases PASS')
