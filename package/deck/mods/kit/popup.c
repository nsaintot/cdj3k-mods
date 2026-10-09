// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/popup.c - the deck's own gui::MessagePopupWidget, with tap-to-dismiss.
 *
 * Built the way the browse-caution path builds one:
 * allocate 0x360, run the ctor with PopupType 0 (NoTitle), pass the message as a
 * juce::StringArray, then addAndMakeVisible it. The ctor sizes itself to the whole
 * screen (setBounds 0,0,0x500,0x2d0) and setMessage lays the frame out around the
 * line count, so there is no geometry to compute here.
 *
 * The app also keeps one popup in the global meow::ObjectMap, reachable via
 * MappedObjPtr::link(), but it is parented to a browse view and draws
 * nowhere while another screen is up. Hence our own instance, parented to whatever
 * registers itself.
 */
#include "kit/popup.h"
#include "juce/juce.h"

#define FN_POPUP_CTOR        ep122_sym(EP122_POPUP_CTOR)  /* MessagePopupWidget(PopupType, const juce::Identifier&) */
#define FN_POPUP_SETMSG      ep122_sym(EP122_POPUP_SETMESSAGE)  /* setMessage(const juce::StringArray&) */
/* addListener(IListener*): scans, then appends. MessagePopupWidget::mouseDown
 * (its own vtable+0x28) walks the listener array at +0x310 (count at +0x320) and calls each listener's vtable slot 0, so a registered listener is
 * enough for tap handling; the shared vtable and stock popups are untouched. */
#define FN_POPUP_ADDLISTENER ep122_sym(EP122_POPUP_ADDLISTENER)
#define FN_IDENT_CTOR        ep122_sym(EP122_JUCE_IDENTIFIER_CTOR)  /* juce::Identifier::Identifier(const char*) */

#define POPUP_ALLOC_SIZE   0x360        /* what the stock call sites hand to operator new         */
#define POPUP_TYPE_NOTITLE 0            /* the plain message variant; setMessage dispatches on it */
#define POPUP_TYPE_OFF     0x328        /* where the ctor stores the PopupType (post-condition)   */

typedef void (*ident_ctor_t)(void *out, const char *utf8);
typedef void (*popup_ctor_t)(void *self, int type, void *ident);
typedef void (*setmsg_t)(void *self, void *strarray);

static uintptr_t g_parent;      /* the Component the popup hangs off              */
static uintptr_t g_popup;       /* our instance, built on first use and kept      */
static int       g_up;          /* it is currently on screen                      */
static int       g_disabled;    /* the ctor post-condition failed: never build again */

void kit_popup_set_parent(uintptr_t comp)
{
    g_parent = comp;
}

/* Every primitive the calls below use. */
static int popup_api_ok(void)
{
    return FN_POPUP_CTOR && FN_POPUP_SETMSG && FN_POPUP_ADDLISTENER && FN_IDENT_CTOR &&
           FN_ADD_VISIBLE && FN_STRARR_CTOR && FN_STRARR_ADD && FN_STRARR_DTOR &&
           FN_STR_CTOR && FN_STR_DTOR;
}

/* The tap listener the popup calls on mouseDown. It carries no state: a bare vtable
 * with slot 0 filled in is a complete IListener for MessagePopupWidget::mouseDown.
 * `g_listener` is the object; its only word is the vtable pointer that
 * `(**listener)(listener)` dereferences.
 *
 * The popup covers the screen and its message lines are non-editable juce::Labels,
 * which do not intercept clicks, so a tap anywhere reaches mouseDown. */
static void popup_tapped(void *self)
{
    (void)self;
    kit_popup_dismiss();
}

static void *const k_listener_vt[] = { (void *)popup_tapped };
static const void *g_listener = k_listener_vt;

/* Build the popup on first use. It cannot be built at install time: the ctor pulls
 * the popup skin and links the font manager, neither of which exists that early.
 * Kept for the process lifetime, so re-showing is a single addAndMakeVisible and the
 * parent never holds a dangling child pointer. */
static uintptr_t popup_get(void)
{
    static uint8_t ident[8] __attribute__((aligned(8)));   /* juce::Identifier: one interned String */
    static int     ident_built;
    uint32_t type = 0xffffffffu;
    void *p;

    if (g_popup) return g_popup;
    if (g_disabled || !g_parent || !popup_api_ok()) {
        MDBG("popup: not shown (parent=%#lx, disabled=%d)\n",
             (unsigned long)g_parent, g_disabled);
        return 0;
    }

    if (!ident_built) {
        /* Interned for good, like the guarded static the stock call sites share. */
        ((ident_ctor_t)FN_IDENT_CTOR)(ident, "MessagePopupWidget");
        ident_built = 1;
    }
    p = malloc(POPUP_ALLOC_SIZE);      /* operator new is malloc underneath, and we never delete */
    if (!p) return 0;
    memset(p, 0, POPUP_ALLOC_SIZE);
    ((popup_ctor_t)FN_POPUP_CTOR)(p, POPUP_TYPE_NOTITLE, ident);

    /* Post-condition: setMessage dispatches on the PopupType field, so a wrong ctor
     * address would send the message to a layout that was never built. */
    if (mod_safe_read((uintptr_t)p + POPUP_TYPE_OFF, &type, sizeof(type)) != 0 ||
        type != POPUP_TYPE_NOTITLE) {
        MDBG("popup: ctor left type=%u at +%#x -> disabled\n", type, POPUP_TYPE_OFF);
        g_disabled = 1;                     /* leaked, but not touched again */
        return 0;
    }
    /* addListener scans before appending, so this stays a single registration. */
    ((void (*)(void *, const void *))FN_POPUP_ADDLISTENER)(p, &g_listener);

    g_popup = (uintptr_t)p;
    MDBG("popup: built at %#lx\n", (unsigned long)g_popup);
    return g_popup;
}

/* setMessage silently does nothing for an empty array or one longer than the line
 * slots it owns, so an out-of-range count is dropped here where it can be logged. */
void kit_popup_show(const char *const *lines, int n)
{
    uint8_t sa[JUCE_STRARRAY_BYTES] __attribute__((aligned(8))) = { 0 };
    uintptr_t popup = popup_get();

    if (!popup) return;
    if (n <= 0 || n > KIT_POPUP_MAX_LINES) {
        MDBG("popup: %d lines out of range 1..%d -> not shown\n", n, KIT_POPUP_MAX_LINES);
        return;
    }

    juce_strarray_set(sa, lines, n);
    ((setmsg_t)FN_POPUP_SETMSG)((void *)popup, sa);
    ((void (*)(void *))FN_STRARR_DTOR)(sa);

    /* addAndMakeVisible sets visible first and then early-returns if it is already
     * our child, so this both shows it the first time and re-shows it afterwards. */
    ((addvis_t)FN_ADD_VISIBLE)((void *)g_parent, (void *)popup, -1);
    g_up = 1;
    MDBG("popup: shown (%d lines)\n", n);
}

int kit_popup_is_up(void)
{
    return g_up;
}

/* The popup is modal: it covers the screen and intercepts clicks. It is dismissed
 * two ways:
 *   - a tap, handled by the listener above;
 *   - hardware input, which bypasses JUCE hit-testing, so the mod owning that
 *     dispatch must call this. */
void kit_popup_dismiss(void)
{
    if (!g_up) return;
    juce_comp_set_visible(g_popup, 0);
    g_up = 0;
    MDBG("popup: dismissed\n");
}
