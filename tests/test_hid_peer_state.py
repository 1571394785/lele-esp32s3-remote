"""Exercise actual peer-state code with a persistent fake NVS across cold boots."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
headers = {
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\nconst char *esp_err_to_name(int);\n",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(tag, ...) ((void)(tag))\n#define ESP_LOGW(tag, ...) ((void)(tag))\n",
    "nvs.h": """#pragma once
#include <stddef.h>
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
int nvs_open(const char *, int, nvs_handle_t *);
int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
int nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
int nvs_commit(nvs_handle_t);
int nvs_erase_key(nvs_handle_t, const char *);
void nvs_close(nvs_handle_t);
""",
    "esp_gatts_api.h": """#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint8_t esp_gatt_if_t;
typedef int esp_gatts_cb_event_t;
enum { ESP_GATTS_WRITE_EVT, ESP_GATTS_CONNECT_EVT, ESP_GATTS_DISCONNECT_EVT, ESP_GATTS_CREAT_ATTR_TAB_EVT };
#define ESP_GATT_OK 0
typedef struct { struct { uint16_t uuid_length; uint8_t *uuid_p; uint16_t length; uint8_t *value; } att_desc; } esp_gatts_attr_db_t;
typedef union {
 struct { int status; uint16_t num_handle; uint16_t *handles; } add_attr_tab;
 struct { uint16_t conn_id; uint8_t remote_bda[6]; } connect;
 struct { uint16_t conn_id, handle, offset, len; uint8_t bda[6]; bool is_prep; uint8_t *value; } write;
} esp_ble_gatts_cb_param_t;
esp_err_t esp_ble_gatts_set_attr_value(uint16_t, uint16_t, const uint8_t *);
""",
    "esp_hidd_gatts.h": "#pragma once\n#include \"esp_gatts_api.h\"\nvoid esp_hidd_gatts_event_handler(esp_gatts_cb_event_t, esp_gatt_if_t, esp_ble_gatts_cb_param_t *);\n",
}
harness = r'''
#include <assert.h>
#include <stdlib.h>
#include "hid_peer_state.c"
typedef struct { char key[14]; peer_record_t value; bool present; } store_t;
static store_t store[4];
static unsigned commits;
static uint16_t applied[128];
const char *esp_err_to_name(int value) { (void)value; return "error"; }
int nvs_open(const char *name, int mode, nvs_handle_t *handle) { (void)name; (void)mode; *handle=1; return 0; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
int nvs_commit(nvs_handle_t handle) { (void)handle; ++commits; return 0; }
static store_t *find(const char *key) {
    for (unsigned i=0;i<4;++i) if (strcmp(store[i].key,key)==0) return &store[i];
    for (unsigned i=0;i<4;++i) if (!store[i].key[0]) { strcpy(store[i].key,key); return &store[i]; }
    abort();
}
int nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size) {
    (void)h; assert(size==sizeof(peer_record_t)); store_t *entry=find(key);
    memcpy(&entry->value,data,size); entry->present=true; return 0;
}
int nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size) {
    (void)h; store_t *entry=find(key); if (!entry->present) return -1;
    memcpy(data,&entry->value,*size); return 0;
}
int nvs_erase_key(nvs_handle_t h, const char *key) { (void)h; find(key)->present=false; return 0; }
esp_err_t __real_esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t *db, esp_gatt_if_t interface, uint16_t count, uint8_t instance) {
    (void)db; (void)interface; (void)count; (void)instance; return 0;
}
esp_err_t esp_ble_gatts_set_attr_value(uint16_t handle, uint16_t length, const uint8_t *data) {
    assert(handle<128); applied[handle]=data[0]; if (length==2) applied[handle]|=(uint16_t)data[1]<<8; return 0;
}
void esp_hidd_gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t *param) {
    (void)interface; if (event==ESP_GATTS_WRITE_EVT && param->write.len==2)
        applied[param->write.handle]=param->write.value[0];
}
static const uint8_t peer_a[6]={1,2,3,4,5,6}, peer_b[6]={6,5,4,3,2,1};
static void boot(void) {
    memset(s_tables,0,sizeof(s_tables)); memset(s_handles,0,sizeof(s_handles));
    memset(s_interfaces,0,sizeof(s_interfaces)); memset(applied,0,sizeof(applied));
    memset(&s_record,0,sizeof(s_record)); s_schema=0; s_active=false; s_authenticated=false;
    s_written=0; s_feature_written=false; s_feature_handle=0;
    uint16_t primary=0x2800, battery=0x180f, hid=0x1812, ccc=0x2902, report=0x2a4d, ref=0x2908;
    uint8_t feature_ref[2]={5,3};
    esp_gatts_attr_db_t bas[2]={0}, db[5]={0};
    bas[0].att_desc=(__typeof__(bas[0].att_desc)){2,(uint8_t *)&primary,2,(uint8_t *)&battery};
    bas[1].att_desc=(__typeof__(bas[0].att_desc)){2,(uint8_t *)&ccc,0,NULL};
    db[0].att_desc=(__typeof__(bas[0].att_desc)){2,(uint8_t *)&primary,2,(uint8_t *)&hid};
    db[1].att_desc=bas[1].att_desc; db[2].att_desc=bas[1].att_desc;
    db[3].att_desc=(__typeof__(bas[0].att_desc)){2,(uint8_t *)&report,0,NULL};
    db[4].att_desc=(__typeof__(bas[0].att_desc)){2,(uint8_t *)&ref,2,feature_ref};
    assert(__wrap_esp_ble_gatts_create_attr_tab(bas,1,2,0)==0);
    assert(__wrap_esp_ble_gatts_create_attr_tab(db,2,5,0)==0);
    uint16_t handles_b[2]={10,11}, handles_h[5]={20,21,22,23,24};
    esp_ble_gatts_cb_param_t event={0}; event.add_attr_tab.num_handle=2; event.add_attr_tab.handles=handles_b;
    hid_peer_gatts_callback(ESP_GATTS_CREAT_ATTR_TAB_EVT,1,&event);
    event.add_attr_tab.num_handle=5; event.add_attr_tab.handles=handles_h;
    hid_peer_gatts_callback(ESP_GATTS_CREAT_ATTR_TAB_EVT,2,&event);
    assert(s_feature_handle==23 && s_handles[0]==11 && s_handles[1]==21 && s_handles[2]==22);
}
static void connect_peer(const uint8_t *address) {
    esp_ble_gatts_cb_param_t event={0}; event.connect.conn_id=7; memcpy(event.connect.remote_bda,address,6);
    hid_peer_gatts_callback(ESP_GATTS_CONNECT_EVT,2,&event);
}
static void write_value(uint16_t handle, uint8_t value, uint16_t length) {
    uint8_t bytes[2]={value,0}; esp_ble_gatts_cb_param_t event={0};
    event.write.conn_id=7; event.write.handle=handle; event.write.value=bytes; event.write.len=length;
    hid_peer_gatts_callback(ESP_GATTS_WRITE_EVT,2,&event);
}
int main(void) {
    boot(); connect_peer(peer_a); assert(!hid_peer_ready());
    write_value(21,1,2); write_value(22,1,2); write_value(23,1,1);
    assert(commits==0); hid_peer_authenticated(peer_a); assert(hid_peer_ready());
    assert(hid_peer_wheel_resolution()==120 && applied[21]==1 && applied[22]==1);
    unsigned saved=commits; write_value(21,1,2); assert(commits==saved);
    boot(); connect_peer(peer_a); assert(applied[21]==0 && !hid_peer_ready());
    hid_peer_authenticated(peer_a); assert(applied[21]==1 && applied[23]==1 && hid_peer_wheel_resolution()==120);
    write_value(21,0,2); write_value(23,0,1); // Explicit host disable also persists.
    boot(); connect_peer(peer_a); hid_peer_authenticated(peer_a);
    assert(applied[21]==0 && applied[22]==1 && hid_peer_wheel_resolution()==1);
    boot(); connect_peer(peer_b); hid_peer_authenticated(peer_b);
    assert(applied[21]==0 && applied[22]==0 && hid_peer_wheel_resolution()==1);
    boot(); connect_peer(peer_b); write_value(21,1,2); // Private address resolves to A.
    hid_peer_authenticated(peer_a); assert(applied[21]==1 && applied[22]==1);
    hid_peer_forget(peer_a); boot(); connect_peer(peer_a); hid_peer_authenticated(peer_a);
    assert(applied[21]==0 && applied[22]==0 && hid_peer_wheel_resolution()==1);
    write_value(21,1,2); write_value(23,1,1);
    boot(); s_schema^=1; connect_peer(peer_a); hid_peer_authenticated(peer_a);
    assert(applied[21]==0 && hid_peer_wheel_resolution()==1);
    // Fresh pairing must not restore notifications/feature cached by an old
    // Windows installation, but must retain writes made on this new link.
    boot(); connect_peer(peer_a); hid_peer_authenticated(peer_a);
    write_value(21,1,2); write_value(22,1,2); write_value(23,1,1);
    boot(); connect_peer(peer_a); write_value(21,1,2);
    hid_peer_new_pairing(peer_a); hid_peer_authenticated(peer_a);
    assert(applied[21]==1 && applied[22]==0 && hid_peer_wheel_resolution()==1);
    boot(); connect_peer(peer_a); hid_peer_authenticated(peer_a);
    assert(applied[21]==1 && applied[22]==0 && hid_peer_wheel_resolution()==1);
    puts("PASS: cold reboot, auth gate, peer isolation, private identity, explicit disable, forgetting and schema change");
}
'''
with tempfile.TemporaryDirectory(prefix="hid_peer_test_") as directory:
    temporary = Path(directory)
    for name, content in headers.items():
        (temporary / name).write_text(content)
    (temporary / "test.c").write_text(harness)
    executable = temporary / "test.exe"
    subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(temporary), "-I", str(root / "main"),
                    str(temporary / "test.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
