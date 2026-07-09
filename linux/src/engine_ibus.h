// VietKi Linux shell — IBus engine type (Phase 4.1).
//
// Unlike the Windows/macOS shells (a global keyboard hook that backspaces and
// retypes), the Linux shell is an IBus *engine*: it renders the syllable being
// composed as an underlined preedit and commits it on a word break. This is the
// only model that works under Wayland, where a process cannot intercept or
// inject another process's keystrokes (PHASE4.1 §1, §6).
//
// The OS-independent core (vietki::Engine) is reused verbatim; this file only
// declares the GObject subclass of IBusEngine that wraps it.
#pragma once

#include <ibus.h>

G_BEGIN_DECLS

#define IBUS_TYPE_VIETKI_ENGINE (ibus_vietki_engine_get_type())

typedef struct _IBusVietKiEngine IBusVietKiEngine;
typedef struct _IBusVietKiEngineClass IBusVietKiEngineClass;

GType ibus_vietki_engine_get_type(void);

G_END_DECLS
