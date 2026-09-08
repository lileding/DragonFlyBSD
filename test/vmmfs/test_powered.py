"""PCI powered is an attribute with separate vnode-change notifications."""
from pathlib import Path
import unittest
from test_regress import COMMON, function, run_c

BASE = Path(__file__).resolve().parents[2] / "sys/vfs/vmmfs"

class Powered(unittest.TestCase):
    def test_state_and_notification(self):
        run_c(COMMON + r"""
struct token { unsigned held; };
struct vmmfs_node { int unused; };
struct kqinfo { int ki_note; };
struct vmmfs_pcislot_powered { struct vmmfs_node node; struct token token;
    struct kqinfo kq; bool value, closed; };
struct knote { void *kn_hook; long kn_fflags, kn_sfflags, kn_flags, kn_data; };
#define NOTE_WRITE 1
#define NOTE_REVOKE 2
#define EV_EOF 4
#define EV_NODATA 8
#define lwkt_gettoken(t) (++(t)->held)
#define lwkt_reltoken(t) (--(t)->held)
static unsigned notes; static long last_hint;
#define KNOTE(list, hint) ((void)(list), ++notes, last_hint = (hint))
void
""" + function("vmmfs_pcislot_powered.c", "vmmfs_pcislot_powered_set") + r"""
static int
""" + function("vmmfs_pcislot_powered.c", "vmmfs_pcislot_powered_load") + r"""
static int
""" + function("vmmfs_pcislot_powered.c", "vmmfs_pcislot_powered_filter") + r"""
static bool
""" + function("vmmfs_pcislot_powered.c", "vmmfs_pcislot_powered_deactivate") + r"""
int main(void) {
    struct vmmfs_pcislot_powered p = {0};
    struct knote a = {.kn_hook=&p, .kn_sfflags=NOTE_WRITE|NOTE_REVOKE};
    struct knote b = a;
    char text[2]; size_t len = 0;
    assert(vmmfs_pcislot_powered_load(&p.node, text, 1, &len)==EOVERFLOW);
    assert(vmmfs_pcislot_powered_load(&p.node, text, 2, &len)==0);
    assert(len==2 && text[0]=='0' && text[1]=='\n');
    vmmfs_pcislot_powered_set(&p, false); assert(notes==0);
    vmmfs_pcislot_powered_set(&p, true); assert(notes==1 && last_hint==NOTE_WRITE);
    assert(vmmfs_pcislot_powered_filter(&a, NOTE_WRITE));
    assert(vmmfs_pcislot_powered_filter(&b, NOTE_WRITE));
    a.kn_fflags=0;
    assert(!vmmfs_pcislot_powered_filter(&a, 0));
    assert(vmmfs_pcislot_powered_load(&p.node, text, 2, &len)==0 && text[0]=='1');
    vmmfs_pcislot_powered_set(&p, true); assert(notes==1);
    vmmfs_pcislot_powered_set(&p, false); assert(notes==2);
    assert(vmmfs_pcislot_powered_filter(&a, NOTE_WRITE));
    assert(!(a.kn_flags & EV_EOF));
    assert(vmmfs_pcislot_powered_deactivate(&p.node));
    assert(last_hint==NOTE_REVOKE && notes==3);
    assert(vmmfs_pcislot_powered_filter(&a, 0) && (a.kn_flags & EV_EOF));
    assert(!p.token.held);
}
""")

    def test_power_boundary_order(self):
        on = function("vmmfs_pcislot.c", "vmmfs_pcislot_power_on")
        self.assertLess(on.index("vmmfs_pcislot_config_power_on"), on.index("vmmfs_pcislot_powered_set"))
        self.assertLess(on.index("vmmfs_pciroot_invalidate_slot"), on.index("vmmfs_pcislot_powered_set"))
        off = function("vmmfs_pcislot.c", "vmmfs_pcislot_power_off")
        self.assertLess(off.index("vmmfs_pcislot_powered_set"), off.index("vmmfs_pcislot_config_power_off"))
        self.assertNotIn("powered_set", function("vmmfs_pcislot.c", "vmmfs_pcislot_reset"))

    def test_old_stream_removed(self):
        self.assertFalse((BASE / "vmmfs_pcislot_events.c").exists())
        self.assertNotIn('"events"', (BASE / "vmmfs_pcislot.c").read_text())
        self.assertIn("EVFILT_VNODE", (BASE / "vmmfs_pcislot_powered.c").read_text())
