// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * juce.h - constructing and calling JUCE objects from C.
 *
 * draw.h covers juce::Graphics and painted surfaces; this file covers the
 * objects.
 *
 * juce::Font, juce::String and juce::var are not trivially copyable, so AAPCS
 * returns them indirectly through a buffer in x8. GCC emits that only for a C
 * return type larger than 16 bytes, hence the padded types below. Only the
 * leading bytes are written, and the caller must still destroy the object.
 */
#ifndef EP122_MOD_JUCE_H
#define EP122_MOD_JUCE_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ---- the two indirectly-returned types (see above) ---- */
typedef struct { uint8_t _pad[32]; } __attribute__((aligned(16))) font_ret_t;
typedef struct { uint8_t _pad[32]; } __attribute__((aligned(16))) str_ret_t;

/* ---- primitives ---- */
#define FN_STR_CTOR        ep122_sym(EP122_JUCE_STRING_CTOR_CSTR)  /* String(String*, const char*) */

/* juce_string_utf8 builds a String of the right byte length from an ASCII
 * placeholder, then writes the UTF-8 bytes into its buffer. The fill is ASCII so
 * the placeholder is one byte per character. The cap is sized for a label. */
#define JUCE_UTF8_MAX      32
#define JUCE_UTF8_FILL     'x'
#define FN_STR_DEFCTOR     ep122_sym(EP122_JUCE_STRING_CTOR_EMPTY) /* String() -- the empty string */
#define FN_STR_DTOR        ep122_sym(EP122_JUCE_STRING_DTOR)       /* ~String(String*)             */
#define FN_FONT_BUILD      ep122_sym(EP122_EP122_FONT_BUILD)       /* the deck's own font, by size */
#define FN_FONT_DTOR       ep122_sym(EP122_JUCE_FONT_DTOR)         /* ~Font(Font*)                 */
#define FN_ADD_VISIBLE     ep122_sym(EP122_JUCE_COMP_ADDVISIBLE)   /* addAndMakeVisible(p, c, z)   */
#define FN_SET_BOUNDS      ep122_sym(EP122_JUCE_COMP_SETBOUNDS)    /* setBounds(x, y, w, h)        */
#define FN_COMP_SETCOLOUR  ep122_sym(EP122_JUCE_COMP_SETCOLOUR)    /* setColour(colourId, Colour)  */
#define FN_STRARR_CTOR     ep122_sym(EP122_JUCE_STRARR_CTOR)       /* StringArray()                */
#define FN_STRARR_ADD      ep122_sym(EP122_JUCE_STRARR_ADD)        /* add() -- moves the string in */
#define FN_STRARR_DTOR     ep122_sym(EP122_JUCE_STRARR_DTOR)       /* ~StringArray()               */

typedef void (*str_ctor_t)(void *out, const char *utf8);
typedef void (*str_dtor_t)(void *s);
typedef void (*addvis_t)(void *parent, void *child, int zpos);
typedef void (*strarr_add_t)(void *arr, void *str);
typedef font_ret_t (*font_build_t)(float height);

/* juce::StringArray size on this build. Opaque: the app's ctor/add/dtor own
 * every field and callers only pass the address. It is larger than a 16-byte
 * juce::Array: the ctor clears +0x00/+0x08/+0x10 and add reads
 * numUsed from +0x10, so the high-water mark is +0x14. Declare it zeroed. */
#define JUCE_STRARRAY_BYTES 32

/* ---- juce::Component ----
 * Vtable slots, from the address point. */
#define JUCE_VT_MOUSEDOWN   0x28
#define JUCE_VT_MOUSEDRAG   0x30
#define JUCE_VT_MOUSEUP     0x38
#define JUCE_VT_SETVISIBLE  0x60
#define JUCE_VT_PAINT       0xd0
#define JUCE_VT_PAINTOVER   0xd8   /* paintOverChildren: after the children */

/* Component's own {x, y, w, h}, four int32. */
#define JUCE_BOUNDS_OFF     0x20

/* The rest of the Component fields a walker needs. */
#define JUCE_PARENT_OFF     0x18
#define JUCE_CHILDREN_OFF   0x40    /* Component**            */
#define JUCE_NCHILD_OFF     0x50    /* int, numUsed           */
#define JUCE_FLAGS_OFF      0xc0
#define JUCE_FLAG_VISIBLE   0x02    /* the bit addChildComponent tests before re-showing */

/* Build a juce::String from a UTF-8 literal. FN_STR_CTOR is CharPointer_ASCII
 * on this build, so anything above U+007F arrives as the Latin-1 characters its
 * encoding is made of; this places the literal's own bytes instead. Pure ASCII
 * takes the plain constructor. */
void juce_string_utf8(void *str, const char *text);

/* Walk the tree. `n` is clamped; a component whose array cannot be read is
 * treated as childless. */
int       juce_comp_nchild(uintptr_t comp);
uintptr_t juce_comp_child(uintptr_t comp, int i);
uintptr_t juce_comp_parent(uintptr_t comp);
uintptr_t juce_comp_root(uintptr_t comp);
int       juce_comp_bounds(uintptr_t comp, int32_t out[4]);
int       juce_comp_visible(uintptr_t comp);

/* Identity by typeinfo. Not by vtable: a widget that is a model first and a
 * juce::Component second has its child pointer at the Component subobject,
 * which carries a secondary vtable. Every vtable in a class's group carries the
 * same typeinfo word, so the primary vtable from the spec is enough to look it
 * up.
 *
 * juce_class_of() takes a vtable address point and returns that word. */
uintptr_t juce_class_of(uintptr_t vt);
uintptr_t juce_comp_class(uintptr_t comp);

/* A live component's class as the raw Itanium mangled name the binary stores
 * (N3gui15TrackListWidgetE). For logging only; identity uses the typeinfo
 * compare. "?" when the object has no readable name. */
const char *juce_comp_class_name(uintptr_t comp, char *buf, size_t cap);
uintptr_t juce_comp_child_of_class(uintptr_t parent, uintptr_t ti);
uintptr_t juce_comp_find_class(uintptr_t root, uintptr_t ti);

/* ---- juce::Label ---- */
#define FN_LABEL_CTOR       ep122_sym(EP122_JUCE_LABEL_CTOR)     /* Label(const String& name, const String& text) */
#define FN_LABEL_SETFONT    ep122_sym(EP122_JUCE_LABEL_SETFONT)  /* Label::setFont(const Font&)                   */
#define FN_LABEL_JUSTIFY    ep122_sym(EP122_JUCE_LABEL_JUSTIFY)  /* Label::setJustificationType(Justification)    */
#define LABEL_VTABLE        ep122_sym(EP122_LABEL)
#define LABEL_FN_PAINT      ep122_sym(EP122_LABEL_PAINT)         /* what LABEL_VTABLE+0xd0 must hold (post-cond)  */
#define LABEL_ALLOC_SIZE    0x1d0

/* A Label's text is a juce::Value that the Label listens to, so setting the
 * Value does all of setText: valueChanged runs textWasChanged() and repaint().
 * No setText address is needed. The offset comes from the ctor, which builds a
 * var from the `text` argument and constructs the Value at word 0x2a. */
#define LABEL_TEXTVALUE_OFF 0x150
#define FN_VAR_FROM_STR     ep122_sym(EP122_JUCE_VAR_CTOR_STRING)  /* juce::var::var(const String&)     */
#define FN_VALUE_SETVALUE   ep122_sym(EP122_JUCE_VALUE_SETVALUE)   /* juce::Value::setValue(const var&) */
#define FN_VAR_DTOR         ep122_sym(EP122_JUCE_VAR_DTOR)         /* juce::var::~var()                 */

#define LBL_COL_BG          0x1000280    /* juce::Label::backgroundColourId */
#define LBL_COL_TEXT        0x1000281
#define LBL_COL_OUTLINE     0x1000282

/* setColour takes a juce::Colour by reference; a Colour is just the ARGB word,
 * so a local uint32 serves. It runs colourChanged(), which repaints. */
void juce_comp_colour(uintptr_t comp, int id, uint32_t argb);

/* Build one Label under `parent`. A non-zero `vptr` replaces the object's
 * vtable pointer with a caller-owned clone, which turns the Label into a button.
 * Zero leaves stock juce::Label, whose mouseDown is an empty stub, so the press
 * falls through to the parent. */
uintptr_t juce_label(uintptr_t parent, const char *text, float font_h,
                     uint32_t bg, uint32_t fg, uintptr_t vptr,
                     int x, int y, int w, int h);

/* Set a Label's text through its juce::Value. juce's ValueSource does not
 * compare, so an unchanged string still triggers a repaint; the caller does the
 * dirty check. */
void juce_label_text(uintptr_t label, const char *text);

/* juce::Justification is one int of flags, taken by reference. juce_label
 * builds every Label centred; use this when two Labels must read as one line,
 * since centring each in its own box leaves an arbitrary gap between them.
 *
 * The box must still fit the text: juce::Label narrows text to its minimum
 * horizontal scale before it clips, so a box a few pixels short shows a
 * condensed typeface. */
#define JUCE_JUSTIFY_CENTRED 0x24   /* horizontallyCentred | verticallyCentred */
#define JUCE_JUSTIFY_BOT_L   0x11   /* bottomLeft:   left | bottom             */
#define JUCE_JUSTIFY_BOT_R   0x12   /* bottomRight:  right | bottom            */
#define JUCE_JUSTIFY_MID_L   0x21   /* centredLeft:  left | verticallyCentred   */
void juce_label_justify(uintptr_t label, int justification);

/* Copy juce::Label's vtable into `out` (VT_CLONE_WORDS words) with the caller's
 * overrides applied, and return the address point to store in an object. The
 * paint slot is checked against Label's own first; if it holds anything else the
 * class or the slot numbering moved, and nothing is cloned. */
#define VT_CLONE_WORDS  66      /* 2 head words + 64 slots: past juce::Label's own */
struct juce_vt_override { unsigned slot; void *fn; uintptr_t *saved; };
uintptr_t juce_label_vt_clone(uintptr_t *out, const struct juce_vt_override *ov, int n);

/* setVisible through the object's own vtable: subclasses override it, including
 * the popup. A comp whose vtable cannot be read is left alone. */
void juce_comp_set_visible(uintptr_t comp, int visible);

/* Invalidate a whole component. Component::repaint(Rectangle<int>) takes the
 * rect as a pointer in x1, not in x1/x2 as the C++ signature suggests. The rect is in the component's own
 * coordinates, so its origin is 0,0. */
#define FN_COMP_REPAINT_RECT ep122_sym(EP122_JUCE_COMP_REPAINT_RECT)
void juce_comp_repaint(uintptr_t comp);

/* Read a juce::String into a plain buffer. The String points to a refcounted
 * UTF-8 body with no reachable length, so the read length is halved from cap-1
 * until it lands inside mapped memory. Returns -1 if no NUL is found in what was
 * read. */
int juce_string_read(uintptr_t str, char *buf, size_t cap);

/* Fill `sa` (>= JUCE_STRARRAY_BYTES, aligned) with `n` lines. add() moves its
 * argument, so each temporary is destroyed afterwards as the shared empty
 * string. */
void juce_strarray_set(void *sa, const char *const *lines, int n);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_JUCE_H */
