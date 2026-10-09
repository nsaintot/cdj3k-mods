/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * call.hh - calling EP122's own code with the signature stated once.
 *
 * Calling a resolved address needs a cast, and a cast at the call site states
 * the signature where the compiler cannot check it: a wrong argument count or
 * type compiles cleanly and corrupts the app at run time. Here the signature is
 * a template parameter, so the argument count is enforced and each argument is
 * converted to the declared type.
 *
 *     ep_call(void(void *, int, int, int, int))::at(MOD_FN_GFX_FILLRECT,
 *                                                   g, x, y, w, h);
 *     ep_call(void(void *))::slot(comp, JUCE_VT_PAINT, g);
 *
 * Slots are byte offsets from the address point, as ep122_syms.spec records
 * them. EP122 classes are not mirrored as C++ classes: the layouts are
 * only partly known, several widget classes reach juce::Component through
 * virtual inheritance, and the compiler's slot assignment could diverge
 * silently.
 */
#ifndef EP122_MOD_CALL_HH
#define EP122_MOD_CALL_HH

#include "core/mod_core.h"

namespace ep {

template <class Sig> struct call_;

template <class R, class... P>
struct call_<R(P...)> {
    /* By address, as ep122_sym() returns it. */
    static inline R at(uintptr_t fn, P... p)
    {
        return reinterpret_cast<R (*)(P...)>(fn)(p...);
    }

    /* Through the object's own vtable. `off` is the byte offset from the
     * address point; `self` becomes the first argument. */
    static inline R slot(uintptr_t self, unsigned off, P... p)
    {
        uintptr_t vt = *reinterpret_cast<uintptr_t *>(self);
        uintptr_t fn = *reinterpret_cast<uintptr_t *>(vt + off);

        return reinterpret_cast<R (*)(uintptr_t, P...)>(fn)(self, p...);
    }

    /* A slot read from a vtable whose address is already known, for a clone. */
    static inline R slot_in(uintptr_t vt, unsigned off, uintptr_t self, P... p)
    {
        uintptr_t fn = *reinterpret_cast<uintptr_t *>(vt + off);

        return reinterpret_cast<R (*)(uintptr_t, P...)>(fn)(self, p...);
    }
};

}  /* namespace ep */

/* Variadic so a signature's own commas survive the preprocessor. */
#define ep_call(...) ::ep::call_<__VA_ARGS__>

#endif /* EP122_MOD_CALL_HH */
