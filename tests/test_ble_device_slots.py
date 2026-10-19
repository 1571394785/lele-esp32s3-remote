"""Run the firmware's actual slot, security and transition functions with fake BLE/NVS."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/ble_hid.c").read_text(encoding="utf-8")
names = ["slot_key", "load_device_slots", "save_selected_slot", "save_device_slot",
         "wait_flag_clear", "stop_advertising_now", "bond_has_irk", "address_matches_bond",
         "get_slot_bond", "address_matches_slot", "selected_filter_identity", "configure_slot_filter",
         "configure_discoverable", "addr_is_any_slot", "addr_is_allowed_for_security",
         "start_advertising_if_ready", "gap_event_handler", "ble_hid_disconnect",
         "ble_hid_select_device_slot", "start_selected_slot", "ble_hid_connect_selected_device",
         "ble_hid_poll", "ble_hid_switch_device_slot", "ble_hid_enter_pairing_mode"]

def extract(name):
    match = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;]*?\)\n\{", source, re.M)
    assert match, name
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]

harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "ble_hid.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_TIMEOUT -3
#define ESP_ERR_NOT_FOUND -4
#define ESP_BT_STATUS_SUCCESS 0
#define ESP_BLE_ID_KEY_MASK 2
#define ESP_BLE_DEVICE_PRIVACY_MODE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e=(expr); if(e) return e; } while(0)
#define BLE_HID_DEVICE_NVS_NAMESPACE "bledev"
#define BLE_HID_DEVICE_NVS_CURRENT "current"
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_BLE_ADV_FLAG_BREDR_NOT_SPT 1
#define ESP_BLE_ADV_FLAG_GEN_DISC 2
#define BLE_WL_ADDR_TYPE_RANDOM 1
#define BLE_WL_ADDR_TYPE_PUBLIC 0
#define ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY 0
#define ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST 1
#define TAG "test"
typedef uint8_t esp_bd_addr_t[6];
typedef unsigned TickType_t;
typedef int nvs_handle_t;
typedef int esp_err_t;
typedef int esp_bt_status_t;
typedef int esp_gap_ble_cb_event_t;
enum {ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT, ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT,
ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT, ESP_GAP_BLE_ADV_START_COMPLETE_EVT,
ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, ESP_GAP_BLE_SEC_REQ_EVT, ESP_GAP_BLE_PASSKEY_NOTIF_EVT,
ESP_GAP_BLE_KEY_EVT, ESP_GAP_BLE_AUTH_CMPL_EVT, ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT, ESP_GAP_BLE_SET_PRIVACY_MODE_COMPLETE_EVT};
typedef struct {
 struct {int status;} local_privacy_cmpl, set_privacy_mode_cmpl, update_whitelist_cmpl, adv_data_cmpl, scan_rsp_data_cmpl, adv_start_cmpl, adv_stop_cmpl;
 struct {
  struct {uint8_t bd_addr[6];} ble_req;
  struct {uint32_t passkey;} key_notif;
  struct {bool success; uint8_t bd_addr[6]; uint8_t addr_type;} auth_cmpl;
 } ble_security;
} esp_ble_gap_cb_param_t;
static volatile bool s_connected,s_link_up,s_transition,s_adv_starting,s_adv_stopping,s_whitelist_busy,s_privacy_busy,s_filter_pending;
static volatile int s_whitelist_status,s_privacy_status;
static bool s_advertising,s_allow_auto_advertise,s_pairing_discoverable,s_pairing_new_keys;
static bool s_adv_config_done=true,s_scan_rsp_config_done=true;
static uint8_t s_selected_device_slot,s_connected_bda[6];
static char s_connected_addr[18];
static ble_hid_device_slot_t s_device_slots[3], stored[3];
static uint8_t stored_selected;
typedef struct { uint8_t bd_addr[6]; struct {unsigned key_mask;struct {uint8_t static_addr[6];uint8_t addr_type;uint8_t irk[16];} pid_key;} bond_key;} esp_ble_bond_dev_t;
static esp_ble_bond_dev_t mock_bond;
static bool have_bond,privacy_enabled,privacy_failure;
int esp_ble_get_bond_device_num(void){return have_bond ? 1 : 0;}
int esp_ble_get_bond_device_list(int*n,esp_ble_bond_dev_t*b){*n=have_bond ? 1 : 0;if(have_bond)*b=mock_bond;return 0;}
bool ble_peer_rpa_matches(const uint8_t*irk,const uint8_t*addr){return irk[0]==99 && addr[0]==0x41 && addr[1]==99;}

static struct {int adv_filter_policy;} s_adv_params;
static struct {int flag;} s_adv_data;
static unsigned ticks,disconnects,removes,starts;
static bool pending_disconnect,timeout_disconnect,fail_nvs,late_auth;
static uint8_t whitelist[6],removed[6];
static void gap_event_handler(int,esp_ble_gap_cb_param_t*);
static void start_advertising_if_ready(void);
static void auth(const uint8_t *addr);
int nvs_open(const char *name,int mode,int *h) {(void)name;(void)mode;*h=1;return fail_nvs ? -1 : 0;}
int nvs_get_u8(int h,const char*k,uint8_t*v){(void)h;(void)k;*v=stored_selected;return 0;}
int nvs_set_u8(int h,const char*k,uint8_t v){(void)h;(void)k;stored_selected=v;return 0;}
int nvs_get_blob(int h,const char*k,void*v,size_t*n){(void)h;assert(*n==sizeof(stored[0]));memcpy(v,&stored[k[4]-'0'],*n);return 0;}
int nvs_set_blob(int h,const char*k,const void*v,size_t n){(void)h;memcpy(&stored[k[4]-'0'],v,n);return 0;}
int nvs_commit(int h){(void)h;return 0;}
void nvs_close(int h){(void)h;}
TickType_t xTaskGetTickCount(void){return ticks;}
void vTaskDelay(unsigned ms){ticks+=ms;if(pending_disconnect&&!timeout_disconnect){
 if(late_auth){late_auth=false;auth(s_device_slots[s_selected_device_slot].addr);}
 pending_disconnect=false;s_link_up=false;s_connected=false;start_advertising_if_ready();}}
int esp_ble_gap_disconnect(const uint8_t*a){(void)a;++disconnects;pending_disconnect=true;return 0;}
int esp_ble_gap_stop_advertising(void){s_advertising=false;s_adv_stopping=false;return 0;}
int esp_ble_gap_start_advertising(void*params){(void)params;assert(!s_link_up);++starts;s_advertising=true;s_adv_starting=false;return 0;}
int esp_ble_gap_config_local_privacy(bool enabled){assert(!s_advertising&&!s_link_up);privacy_enabled=enabled;s_privacy_status=privacy_failure?-1:0;s_privacy_busy=false;return 0;}
int esp_ble_gap_set_privacy_mode(uint8_t type,uint8_t*addr,int mode){(void)type;(void)addr;assert(!s_link_up&&privacy_enabled&&mode==ESP_BLE_DEVICE_PRIVACY_MODE);s_privacy_status=0;s_privacy_busy=false;return 0;}
int esp_ble_gap_clear_whitelist(void){memset(whitelist,0,6);s_whitelist_status=0;s_whitelist_busy=false;return 0;}
int esp_ble_gap_update_whitelist(bool add,uint8_t*a,int type){(void)add;(void)type;memcpy(whitelist,a,6);s_whitelist_status=0;s_whitelist_busy=false;return 0;}
int esp_ble_gap_config_adv_data(void*p){(void)p;s_adv_config_done=true;return 0;}
void esp_ble_gap_security_rsp(const uint8_t*a,bool ok){(void)a;(void)ok;}
void hid_peer_new_pairing(const uint8_t *addr){(void)addr;}
void hid_peer_authenticated(const uint8_t*a){(void)a;}
static void remove_bond_for_addr(const uint8_t*a){++removes;memcpy(removed,a,6);}
'''
tests = r'''
static void auth(const uint8_t *addr){esp_ble_gap_cb_param_t p={0};p.ble_security.auth_cmpl.success=true;memcpy(p.ble_security.auth_cmpl.bd_addr,addr,6);gap_event_handler(ESP_GAP_BLE_AUTH_CMPL_EVT,&p);}
int main(void){
 const uint8_t a[6]={1,2,3,4,5,6},b[6]={2,3,4,5,6,7},c[6]={0x41,4,5,6,7,8};
 // First pairing and reconnecting a selected host.
 assert(ble_hid_switch_device_slot(0)==0 && s_pairing_discoverable);
 s_link_up=true;auth(a);assert(s_connected && stored[0].valid);
 assert(!s_pairing_discoverable && s_filter_pending && !s_allow_auto_advertise);
 // Pairing does not alter controller filters while the HID link is live.
 unsigned paired_disconnects=disconnects;
 auth(a); // A repeated auth notification on the live new bond remains valid.
 assert(s_connected && disconnects==paired_disconnects);
 ble_hid_poll();
 assert(s_link_up && s_filter_pending && disconnects==paired_disconnects);
 // Simulate the real link-down event rather than a user disconnect.
 s_link_up=false;s_connected=false;s_advertising=false;
 ble_hid_poll();
 assert(!s_filter_pending && s_advertising);
 assert(!privacy_enabled && !memcmp(whitelist,a,6));
 s_link_up=true;auth(a);
 unsigned before=starts;
 // A late authentication while disconnecting cannot populate the next slot.
 late_auth=true;assert(ble_hid_switch_device_slot(1)==0);
 assert(!s_device_slots[1].valid && s_pairing_discoverable && starts==before+1);
 s_link_up=true;auth(b);assert(stored[1].valid && stored[0].valid);
 assert(ble_hid_switch_device_slot(2)==0);s_link_up=true;auth(c);
 assert(stored[0].valid && stored[1].valid && stored[2].valid);
 // All three slots reconnect exclusively to their authenticated identity.
 for(unsigned i=0;i<3;++i){assert(ble_hid_switch_device_slot(i)==0);
  assert(!s_pairing_discoverable && s_adv_params.adv_filter_policy==ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST);
  assert(!memcmp(whitelist,stored[i].addr,6));
  assert(addr_is_allowed_for_security(stored[i].addr));
  assert(!addr_is_allowed_for_security(stored[(i+1)%3].addr));
  s_link_up=true;auth(stored[i].addr);assert(s_connected);
 }
 // Re-selecting the connected slot does not drop the link.
 before=disconnects;assert(ble_hid_switch_device_slot(2)==0 && disconnects==before);
 assert(ble_hid_switch_device_slot(3)==ESP_ERR_INVALID_ARG && s_selected_device_slot==2);
 // Selection and device bindings survive a cold state reload.
 memset(s_device_slots,0,sizeof(s_device_slots));s_selected_device_slot=0;load_device_slots();
 assert(s_selected_device_slot==2 && s_device_slots[0].valid && s_device_slots[1].valid && s_device_slots[2].valid);
 // Repair only the current slot, retaining both other computers and identity.
 assert(ble_hid_enter_pairing_mode()==0 && removes==1 && !memcmp(removed,c,6));
 assert(!stored[2].valid && stored[0].valid && stored[1].valid && s_selected_device_slot==2);
 assert(addr_is_allowed_for_security(a));
 // A wrong already-paired computer cannot occupy the empty slot.
 s_link_up=true;auth(a);assert(!s_connected && !s_device_slots[2].valid);
 vTaskDelay(10);assert(s_pairing_discoverable && !s_allow_auto_advertise);
 before=starts;start_advertising_if_ready();assert(starts==before);
 assert(ble_hid_enter_pairing_mode()==0);
 s_link_up=true;auth(c);assert(s_connected);
 // A user disconnect cancels deferred auto-reconnect after fresh pairing.
 assert(s_filter_pending);
 assert(ble_hid_disconnect()==0 && !s_filter_pending);
 before=starts;ble_hid_poll();assert(starts==before);
 // Rotating iPhone addresses must select the same slot, while other hosts
 // remain rejected by the authentication and security gate.
 have_bond=true;memcpy(mock_bond.bd_addr,c,6);mock_bond.bond_key.key_mask=ESP_BLE_ID_KEY_MASK;
 mock_bond.bond_key.pid_key.irk[0]=99;
 const uint8_t identity[6]={9,8,7,6,5,4},rpa[6]={0x41,99,2,3,4,5},wrong_rpa[6]={0x41,98,2,3,4,5};
 memcpy(mock_bond.bond_key.pid_key.static_addr,identity,6);
 assert(ble_hid_switch_device_slot(0)==0);
 assert(!addr_is_allowed_for_security(rpa));
 assert(ble_hid_switch_device_slot(2)==0);
 assert(s_adv_params.adv_filter_policy==ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST);
 assert(privacy_enabled&&!memcmp(whitelist,identity,6));
 // Old Windows host retries are rejected by the simulated controller before
 // any connect/auth callback. No software disconnect/re-advertise loop occurs.
 unsigned rejected=0,old_disconnects=disconnects;
 for(unsigned retry=0;retry<1000;++retry){
  bool admitted=!memcmp(whitelist,a,6) || (have_bond && ble_peer_rpa_matches(mock_bond.bond_key.pid_key.irk,a));
  if(!admitted)++rejected;
 }
 assert(rejected==1000&&disconnects==old_disconnects);
 assert(ble_peer_rpa_matches(mock_bond.bond_key.pid_key.irk,rpa));
 assert(addr_is_allowed_for_security(rpa) && addr_is_allowed_for_security(identity));
 assert(!addr_is_allowed_for_security(wrong_rpa) && !addr_is_allowed_for_security(a));
 s_link_up=true;auth(rpa);assert(s_connected);
 assert(!memcmp(stored[2].addr,c,6));
 // Re-pairing a host in another position requires a fresh key exchange.
 assert(ble_hid_switch_device_slot(0)==0);
 assert(ble_hid_enter_pairing_mode()==0);
 s_link_up=true;esp_ble_gap_cb_param_t key_event={0};
 gap_event_handler(ESP_GAP_BLE_KEY_EVT,&key_event);
 before=disconnects;auth(b);
 assert(s_connected && disconnects==before && s_filter_pending);
 assert(stored[0].valid && !memcmp(stored[0].addr,b,6));
 assert(!stored[1].valid && stored[2].valid);
 assert(ble_hid_switch_device_slot(2)==0);s_link_up=true;auth(rpa);assert(s_connected);
 // Timeout must leave the old selection intact and advertising disabled.
 timeout_disconnect=true;assert(ble_hid_switch_device_slot(0)==ESP_ERR_TIMEOUT);
 assert(s_selected_device_slot==2 && !s_allow_auto_advertise && !s_transition);
 timeout_disconnect=false;vTaskDelay(10);
 // Storage failure must not change the selection.
 fail_nvs=true;assert(ble_hid_switch_device_slot(0)!=0 && s_selected_device_slot==2);
 fail_nvs=false;privacy_failure=true;
 assert(ble_hid_switch_device_slot(0)!=0);
 assert(!s_allow_auto_advertise&&!s_advertising);
 assert(s_adv_params.adv_filter_policy==ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST);
 puts("PASS: three pairings, targeted reconnect, late auth, reboot persistence, selective repair, wrong host, fresh-key host relocation, paused rejected pairing, deferred filtering, controller RPA whitelist, 1000 blocked old-host retries, fail-closed privacy, timeout, NVS failure");
}
'''
with tempfile.TemporaryDirectory(prefix="ble_slots_test_") as temp:
    temp = Path(temp)
    (temp / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    (temp / "test.c").write_text(harness + "\n".join(map(extract, names)) + tests, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(temp), "-I", str(root / "main"), str(temp / "test.c"), "-o", str(temp / "test.exe")], check=True)
    subprocess.run([str(temp / "test.exe")], check=True)
