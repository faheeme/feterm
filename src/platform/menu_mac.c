/*
 * SDL's default macOS menu bar gives Cmd+W to "Window > Close", which would
 * close the whole window (and so quit) before FeTerm sees the key. Clearing
 * that menu item's key equivalent lets Cmd+W reach the keymap (close_pane);
 * the red close button and Cmd+Q still close the window. Plain C through the
 * Objective-C runtime, so no Objective-C source is needed.
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>

#include "util/log.h"

bool plat_macos_free_close_key(void);

typedef id (*msg_obj)(id, SEL);
typedef id (*msg_obj_index)(id, SEL, long);
typedef id (*msg_obj_cstr)(id, SEL, const char *);
typedef long (*msg_long)(id, SEL);
typedef SEL (*msg_sel)(id, SEL);
typedef void (*msg_set_obj)(id, SEL, id);
typedef void (*msg_void)(id, SEL);

static id send_obj(id target, const char *sel)
{
    return target != nullptr ? ((msg_obj)objc_msgSend)(target, sel_registerName(sel)) : nullptr;
}

static long item_count(id menu)
{
    return menu != nullptr ? ((msg_long)objc_msgSend)(menu, sel_registerName("numberOfItems")) : 0;
}

static id item_at(id menu, long i)
{
    return ((msg_obj_index)objc_msgSend)(menu, sel_registerName("itemAtIndex:"), i);
}

bool plat_macos_free_close_key(void)
{
    Class app_class = objc_getClass("NSApplication");
    Class str_class = objc_getClass("NSString");
    if (app_class == nullptr || str_class == nullptr) {
        return false;
    }
    id menu = send_obj(send_obj((id)app_class, "sharedApplication"), "mainMenu");
    long n = item_count(menu);
    if (n <= 0) {
        return false;
    }
    id empty = ((msg_obj_cstr)objc_msgSend)(send_obj((id)str_class, "alloc"), sel_registerName("initWithUTF8String:"), "");
    if (empty == nullptr) {
        return false;
    }
    const SEL close = sel_registerName("performClose:");
    bool done = false;
    for (long i = 0; i < n && i < 64; i++) {
        id sub = send_obj(item_at(menu, i), "submenu");
        long m = item_count(sub);
        for (long j = 0; j < m && j < 64; j++) {
            id item = item_at(sub, j);
            if (item != nullptr && ((msg_sel)objc_msgSend)(item, sel_registerName("action")) == close) {
                ((msg_set_obj)objc_msgSend)(item, sel_registerName("setKeyEquivalent:"), empty);
                done = true;
            }
        }
    }
    ((msg_void)objc_msgSend)(empty, sel_registerName("release"));   /* the menu item keeps its own copy */
    return done;
}
