"""Check actual BMI220 waits against every scheduler phase, including 100 Hz."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/bmi270.c").read_text(encoding="utf-8")
start = source.index("static void bmi220_wait_ms(")
end = source.index("\n}", start) + 2
helper = source[start:end]
assert "vTaskDelay(pdMS_TO_TICKS(" not in source
main = (root / "main/main.c").read_text(encoding="utf-8")
assert main.index("ret = bmi270_init();") < main.index("ret = buttons_init();")
with tempfile.TemporaryDirectory(prefix="bmi220_wait_") as directory:
    path = Path(directory)
    test = path / "test.c"
    test.write_text("""
#include <stdint.h>
#include <assert.h>
typedef uint32_t TickType_t;
static TickType_t waited;
static void vTaskDelay(TickType_t ticks) { waited = ticks; }
""" + helper + """
int main(void) {
    const unsigned delays[] = {1, 2, 4, 12, 100, 140};
    for (unsigned i = 0; i < sizeof(delays)/sizeof(delays[0]); ++i) {
        bmi220_wait_ms(delays[i]);
        // The first tick can arrive immediately; only subsequent ticks count.
        assert((uint64_t)(waited - 1) * 1000 >=
               (uint64_t)delays[i] * configTICK_RATE_HZ);
    }
    return 0;
}
""", encoding="utf-8")
    for hz in (100, 250, 1000):
        exe = path / f"test_{hz}.exe"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DconfigTICK_RATE_HZ={hz}", str(test), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("BMI220 startup waits: minimum delays and initialization order passed")
