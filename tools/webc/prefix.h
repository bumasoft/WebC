// Where the framework lives, resolved at run time — the one fact an installed CLI
// cannot have baked in at compile time.
//
// A checkout knows (the build bakes WEBC_PREFIX), but a release tree moves: it is
// copied, untarred, symlinked from ~/.local/bin. The resolution order is therefore
// the env that says where, then the tree beside the binary (through symlinks), then
// the path the build baked. Call webc_prefix_init(argv[0]) once, from main().
#ifndef WEBC_PREFIX_H
#define WEBC_PREFIX_H

void webc_prefix_init(const char *argv0); // once, early: remembers argv[0]
const char *webc_prefix(void);            // the framework root, always non-NULL

#endif
