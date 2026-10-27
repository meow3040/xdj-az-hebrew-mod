/* SPDX-License-Identifier: GPL-3.0-only */
/* link_members.c: the PRO DJ LINK player numbers present, from EP147's NetworkManager.
 * #included by azmod.c.
 *
 * NetworkManager::onLinkMemberAdded (slot 9) and onLinkMemberRemoved (slot 10) receive the
 * member record whose first byte is the player number EP147 logs ("ADD: ID = 3"; its getter
 * FUN_0131fd48 is one ldrb). Both are
 * reached only through this vtable. The added hook records a member only when EP147 did:
 * EP147 returns early, without notifying anyone, while its byte +0x542 is set.
 * Hooks run on EP147's link thread; readers use g_lm_players with acquire loads. */
#include "link_members.h"

#define LM_NM_VPTR EP_28729f8      /* network::NetworkManager */
#define LM_ADDED_SLOT (LM_NM_VPTR + 0x48)
#define LM_REMOVED_SLOT (LM_NM_VPTR + 0x50)

static uint64_t g_lm_players; /* bit n: player n present (lm_note) */
static int g_lm_watch;        /* 1 once both hooks are in */
static void *g_lm_orig_added, *g_lm_orig_removed;

static uint32_t lm_player_of(uintptr_t member) {
    if (!ep_addr_ok(member & ~(uintptr_t) 7))
        return 0;
    return *(const volatile uint8_t *) member; /* the caller's live argument */
}

static void lm_log(const char *what, uintptr_t member, uint32_t player) {
    FILE *f = azlog_open("/tmp/azmod-linkmembers.log", "a");
    if (f) {
        fprintf(f, "[%ld] %s member %lx player %u -> set %llx\n", (long) time(NULL), what,
                (unsigned long) member, player,
                (unsigned long long) __atomic_load_n(&g_lm_players, __ATOMIC_ACQUIRE));
        fclose(f);
    }
}

static void lm_added_hook(uintptr_t self, uintptr_t member, uintptr_t ip) {
    uint32_t p = lm_player_of(member);
    ((void (*)(uintptr_t, uintptr_t, uintptr_t)) g_lm_orig_added)(self, member, ip);
    if (!*(const volatile uint8_t *) (self + 0x542))
        lm_note(&g_lm_players, p, 1);
    lm_log("added", member, p);
}

static int lm_removed_hook(uintptr_t self, uintptr_t member, uintptr_t ip, int flag) {
    uint32_t p = lm_player_of(member);
    lm_note(&g_lm_players, p, 0);
    lm_log("removed", member, p);
    return ((int (*)(uintptr_t, uintptr_t, uintptr_t, int)) g_lm_orig_removed)(self, member, ip,
                                                                               flag);
}

static uint64_t lm_players(void) {
    return __atomic_load_n(&g_lm_players, __ATOMIC_ACQUIRE);
}
static int lm_watch_ok(void) {
    return __atomic_load_n(&g_lm_watch, __ATOMIC_ACQUIRE);
}

/* Constructor phase (install_static_hooks): slot patches only, each slot checked to hold the
 * expected function first. */
static void install_link_members(void) {
    uintptr_t a0 = 0, r0 = 0;
    int a = -2, r = -2;
    if (img_q(LM_ADDED_SLOT, &a0) && a0 == EP_1349130)
        a = azm_patch_vtable(LM_ADDED_SLOT, (void *) lm_added_hook, &g_lm_orig_added);
    if (a == 1 && img_q(LM_REMOVED_SLOT, &r0) && r0 == EP_1348c48)
        r = azm_patch_vtable(LM_REMOVED_SLOT, (void *) lm_removed_hook, &g_lm_orig_removed);
    __atomic_store_n(&g_lm_watch, a == 1 && r == 1, __ATOMIC_RELEASE);
    FILE *f = azlog_open("/tmp/azmod-linkmembers.log", "a");
    if (f) {
        fprintf(f, "install link members: added %d removed %d\n", a, r);
        fclose(f);
    }
}
