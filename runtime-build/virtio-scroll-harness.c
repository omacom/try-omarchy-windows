/* Portable harness for the actual patched virtio BTN/REL dispatch. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <limits.h>

enum { INPUT_EVENT_KIND_BTN, INPUT_EVENT_KIND_REL };
enum { INPUT_BUTTON_WHEEL_UP, INPUT_BUTTON_WHEEL_DOWN,
       INPUT_BUTTON_WHEEL_LEFT, INPUT_BUTTON_WHEEL_RIGHT, INPUT_BUTTON_LEFT };
enum { INPUT_AXIS_X, INPUT_AXIS_Y, INPUT_AXIS_WHEEL, INPUT_AXIS_HWHEEL };
enum { EV_KEY = 1, EV_REL = 2, REL_X=0, REL_Y=1, REL_WHEEL = 8, REL_HWHEEL = 6,
       REL_WHEEL_HI_RES = 11, REL_HWHEEL_HI_RES = 12, VIRTIO_INPUT_CFG_EV_BITS = 0x11 };
typedef struct { uint16_t type, code; uint32_t value; } virtio_input_event;
typedef struct { uint8_t size; union { uint8_t bitmap[128]; } u; } virtio_input_config;
typedef struct { virtio_input_config *rel; } VirtIOInput;
typedef struct { int64_t wheel_remainder, hwheel_remainder; } VirtIOInputHID;
typedef struct { int button; bool down; } InputBtnEvent;
typedef struct { int axis; int64_t value; } InputMoveEvent;
typedef struct { int type; union {
    struct { InputBtnEvent *data; } btn;
    struct { InputMoveEvent *data; } rel;
} u; } InputEvent;

static const unsigned short keymap_button[] = { 0, 0, 0, 0, 0x110 };
static const unsigned short axismap_rel[] = {REL_X, REL_Y, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES};
static virtio_input_event events[8];
static size_t count;
#define cpu_to_le16(value) ((uint16_t)(value))
#define cpu_to_le32(value) ((uint32_t)(value))
static const char *InputButton_str(int button) { (void)button; return "unknown"; }
static virtio_input_config *virtio_input_find_config(VirtIOInput *device, uint8_t select, uint8_t subsel)
{
    assert(select == VIRTIO_INPUT_CFG_EV_BITS && subsel == EV_REL);
    return device->rel;
}
static void virtio_input_send(VirtIOInput *device, virtio_input_event *event)
{
    (void)device;
    assert(count < sizeof(events) / sizeof(events[0]));
    events[count++] = *event;
}
/* REL_CAPABILITY */
static void handle(VirtIOInput *vinput, VirtIOInputHID *vhid, InputEvent *evt)
{
    virtio_input_event event;
    InputBtnEvent *btn;
    InputMoveEvent *move;
    count = 0;
    switch (evt->type) {
    /* WHEEL_DISPATCH */
    }
}
static void check(size_t index, int code, int value)
{
    assert(index < count);
    assert(events[index].type == EV_REL && events[index].code == code);
    assert((int32_t)events[index].value == value);
}
int main(void)
{
    virtio_input_config config = { .size = 2,
        .u.bitmap = { 1 << REL_HWHEEL, 1 | (1 << 3) | (1 << 4) } };
    VirtIOInput device = { .rel = &config };
    VirtIOInputHID hid = {0};
    InputMoveEvent move = {INPUT_AXIS_WHEEL, 30};
    InputBtnEvent btn = {INPUT_BUTTON_WHEEL_UP, true};
    InputEvent precise = {.type=INPUT_EVENT_KIND_REL, .u.rel.data=&move};
    InputEvent mouse = {.type=INPUT_EVENT_KIND_BTN, .u.btn.data=&btn};
    handle(&device, &hid, &precise);
    assert(count==1 && hid.wheel_remainder==30); check(0,REL_WHEEL_HI_RES,30);
    for (int button=0; button<4; button++) {
        btn.button=button; btn.down=true;
        handle(&device,&hid,&mouse);
        assert(count==2 && hid.wheel_remainder==30);
        int sign=button==INPUT_BUTTON_WHEEL_UP || button==INPUT_BUTTON_WHEEL_RIGHT ? 1 : -1;
        check(0,button<2?REL_WHEEL_HI_RES:REL_HWHEEL_HI_RES,sign*120);
        check(1,button<2?REL_WHEEL:REL_HWHEEL,sign);
        btn.down=false; handle(&device,&hid,&mouse); assert(count==0);
    }
    move.value=90; handle(&device,&hid,&precise);
    assert(count==2 && hid.wheel_remainder==0); check(0,REL_WHEEL_HI_RES,90); check(1,REL_WHEEL,1);
    move.axis=INPUT_AXIS_HWHEEL; move.value=-30; handle(&device,&hid,&precise);
    assert(count==1 && hid.hwheel_remainder==-30); check(0,REL_HWHEEL_HI_RES,-30);
    move.value=15; handle(&device,&hid,&precise); assert(count==1 && hid.hwheel_remainder==-15);
    move.value=-345; handle(&device,&hid,&precise);
    assert(count==2 && hid.hwheel_remainder==0); check(1,REL_HWHEEL,-3);
    move.axis=INPUT_AXIS_WHEEL; move.value=INT_MAX;
    for(int i=0;i<3;i++) {
        int64_t total=hid.wheel_remainder+INT_MAX;
        handle(&device,&hid,&precise); assert(count==2);
        check(0,REL_WHEEL_HI_RES,INT_MAX); check(1,REL_WHEEL,total/120);
        assert(hid.wheel_remainder==total%120);
    }
    /* QAPI values use int64, but the guest wheel payload is signed 32-bit. */
    hid.wheel_remainder=0; move.value=INT64_MAX;
    handle(&device,&hid,&precise); assert(count==1); check(0,REL_WHEEL_HI_RES,-1);
    hid.wheel_remainder=0;
    /* A device without hires bits sends only legacy events, at boundaries. */
    config.u.bitmap[1]=1; hid.wheel_remainder=0;
    move.value=30; handle(&device,&hid,&precise); assert(count==0 && hid.wheel_remainder==30);
    move.value=90; handle(&device,&hid,&precise); assert(count==1); check(0,REL_WHEEL,1);
    btn.button=INPUT_BUTTON_WHEEL_DOWN; btn.down=true;
    handle(&device,&hid,&mouse); assert(count==1); check(0,REL_WHEEL,-1);
    config.size=1; handle(&device,&hid,&mouse); assert(count==0);
    device.rel=NULL; handle(&device,&hid,&precise); assert(count==0);
    handle(&device,&hid,&mouse); assert(count==0);
    btn.button=INPUT_BUTTON_LEFT; handle(&device,&hid,&mouse);
    assert(count==1 && events[0].type==EV_KEY && events[0].code==0x110);
    move.axis=INPUT_AXIS_X; move.value=42; handle(&device,&hid,&precise);
    assert(count==1); check(0,REL_X,42);
    return 0;
}
