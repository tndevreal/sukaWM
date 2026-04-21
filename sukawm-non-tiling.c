//yes, i kno the code is messy and full of unused garbage left behind
//just dont question it
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
static char *autorun_cmds = NULL;
#define CONFIG_PATH_SUFFIX     "/.config/suka/sukawm.suconf"
#define WALLPAPER_PATH_SUFFIX  "/.config/suka/wall.png"

typedef struct Client {
    Window win;
    int x, y;      //canvas cordinates
    int w, h;      //width, height

    int is_fullscreen;
    int saved_x, saved_y, saved_w, saved_h;

    int is_pinned;     //bar
    int is_wallpaper;  //wallpaper

    struct Client *next;
} Client;

typedef enum {
    ACTION_EXEC,
    ACTION_MOVECANVAS,
    ACTION_MOVEWINDOW,
    ACTION_RESIZEWINDOW,
    ACTION_KILLWINDOW,
    ACTION_FOCUSNEXT,
    ACTION_FOCUSPREV,
    ACTION_RAISEWINDOW,
    ACTION_CENTERWINDOW,
    ACTION_TOGGLEFULLSCREEN,
    ACTION_CLIPBOARD_SYNC
} ActionType;

typedef struct Binding {
    int is_mouse;          //0=key, 1=mouse
    unsigned int modifiers;
    int keycode;           
    int button;            
    ActionType action;
    char *cmd;             
    struct Binding *next;
} Binding;

typedef enum {
    DRAG_NONE,
    DRAG_CANVAS,
    DRAG_WINDOW_MOVE,
    DRAG_WINDOW_RESIZE
} DragMode;

typedef struct Var {
    char *name;
    char *value;
    struct Var *next;
} Var;

static Display *dpy;
static Window root;

static Client *clients = NULL;
static Client *focused = NULL;

static Binding *bindings = NULL;
static Var *vars = NULL;


static int canvas_x = 0;
static int canvas_y = 0;

//drag state
static DragMode drag_mode = DRAG_NONE;
static int drag_start_root_x = 0;
static int drag_start_root_y = 0;
static int drag_start_canvas_x = 0;
static int drag_start_canvas_y = 0;
static Client *drag_client = NULL;
static int drag_client_start_x = 0;
static int drag_client_start_y = 0;
static int drag_client_start_w = 0;
static int drag_client_start_h = 0;

//protocols
static Atom wm_protocols;
static Atom wm_delete_window;


//util

static int xerror_handler(Display *d, XErrorEvent *e) {
    (void)d;
    (void)e;
    return 0;
}

static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        //rape all children
    }
}

static void spawn(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (fork() == 0) {
        if (dpy) {
            close(ConnectionNumber(dpy));
        }
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(1);
    }
}

//background support
static void set_background(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", home, WALLPAPER_PATH_SUFFIX);

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "feh --bg-fill '%s'", path);
    spawn(cmd);
}


static void clipboard_sync(void) {
    spawn("xclip -selection primary -o 2>/dev/null | xclip -selection clipboard");
}



static void set_var(const char *name, const char *value) {
    if (!name || !*name || !value) return;
    Var *v = calloc(1, sizeof(Var));
    if (!v) return;
    v->name = strdup(name);
    v->value = strdup(value);
    v->next = vars;
    vars = v;
}

static const char *get_var(const char *name) {
    for (Var *v = vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0)
            return v->value;
    }
    return NULL;
}

static void expand_vars(char *buf, size_t buflen) {
    char tmp[1024];
    char *out = tmp;
    char *end = tmp + sizeof(tmp) - 1;

    for (char *p = buf; *p && out < end; ) {
        if (*p == '$') {
            p++;
            char varname[128];
            int i = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < (int)sizeof(varname)-1) {
                varname[i++] = *p++;
            }
            varname[i] = '\0';
            const char *val = get_var(varname);
            if (val) {
                while (*val && out < end) {
                    *out++ = *val++;
                }
            }
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    strncpy(buf, tmp, buflen);
    buf[buflen-1] = '\0';
}

//client management
static Client *find_client(Window w) {
    for (Client *c = clients; c; c = c->next) {
        if (c->win == w) return c;
    }
    return NULL;
}

static void focus_client(Client *c) {
    if (!c) return;
    focused = c;
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, c->win);
}

static void update_client_geometry(Client *c, int x, int y, int w, int h) {
    if (!c) return;
    c->x = x;
    c->y = y;
    c->w = (w > 1) ? w : 1;
    c->h = (h > 1) ? h : 1;
    int sx = c->x - canvas_x;
    int sy = c->y - canvas_y;
    XMoveResizeWindow(dpy, c->win, sx, sy, (unsigned int)c->w, (unsigned int)c->h);
}

static void restack_special_clients(void) {

    for (Client *c = clients; c; c = c->next) {
        if (c->is_wallpaper) {
            XLowerWindow(dpy, c->win);
        }
    }
    for (Client *c = clients; c; c = c->next) {
        if (c->is_pinned) {
            XRaiseWindow(dpy, c->win);
        }
    }
}

static void reposition_all_clients(void) {
    for (Client *c = clients; c; c = c->next) {
        int sx = c->x - canvas_x;
        int sy = c->y - canvas_y;
        XMoveResizeWindow(dpy, c->win, sx, sy, (unsigned int)c->w, (unsigned int)c->h);
    }
    restack_special_clients();
    XSync(dpy, False);
}


static void grab_buttons_for_window(Window w) {
    for (Binding *b = bindings; b; b = b->next) {
        if (!b->is_mouse) continue;
        XGrabButton(dpy,
                    b->button,
                    b->modifiers,
                    w,
                    True,
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync,
                    GrabModeAsync,
                    None,
                    None);
    }
}

static void manage_window(Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect) {
        return;
    }

    Client *c = (Client *)calloc(1, sizeof(Client));
    if (!c) return;

    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    int lx = wa.x + canvas_x;
    int ly = wa.y + canvas_y;
    int lw = (wa.width > 1) ? wa.width : 1;
    int lh = (wa.height > 1) ? wa.height : 1;

    if (wa.width >= sw && wa.height >= sh) {
        lw = (int)(sw * 0.8);
        lh = (int)(sh * 0.8);
        lx = canvas_x + (sw - lw) / 2;
        ly = canvas_y + (sh - lh) / 2;
    }

    c->win = w;
    c->x = lx;
    c->y = ly;
    c->w = lw;
    c->h = lh;
    c->is_fullscreen = 0;
    c->is_pinned = 0;
    c->is_wallpaper = 0;


    char *name = NULL;
    if (XFetchName(dpy, w, &name) && name) {
        if (strstr(name, "bar")) {
            c->is_pinned = 1;
        }
        if (strstr(name, "feh")) {
            c->is_wallpaper = 1;
        }
        XFree(name);
    }

    c->next = clients;
    clients = c;

    XSelectInput(dpy, w,
                 EnterWindowMask |
                 FocusChangeMask |
                 PropertyChangeMask |
                 StructureNotifyMask |
                 ButtonPressMask |
                 ButtonReleaseMask |
                 PointerMotionMask);

    grab_buttons_for_window(w);

    int sx = c->x - canvas_x;
    int sy = c->y - canvas_y;
    XMoveResizeWindow(dpy, w, sx, sy, (unsigned int)c->w, (unsigned int)c->h);
    XMapWindow(dpy, w);

    if (c->is_wallpaper) {
        XLowerWindow(dpy, w);
    } else if (c->is_pinned) {
        XRaiseWindow(dpy, w);
    }

    focus_client(c);
}

static void unmanage_window(Window w) {
    Client **pc = &clients;
    while (*pc) {
        if ((*pc)->win == w) {
            Client *c = *pc;
            *pc = c->next;
            if (focused == c) focused = NULL;
            free(c);
            return;
        }
        pc = &(*pc)->next;
    }
}



static void kill_focused(void) {
    if (!focused) return;

    Atom *protocols = NULL;
    int n = 0;
    int supports_delete = 0;

    if (XGetWMProtocols(dpy, focused->win, &protocols, &n)) {
        for (int i = 0; i < n; ++i) {
            if (protocols[i] == wm_delete_window) {
                supports_delete = 1;
                break;
            }
        }
        XFree(protocols);
    }

    if (supports_delete) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = focused->win;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = wm_delete_window;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, focused->win, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, focused->win);
    }
}

static void focus_next(void) {
    if (!clients) return;
    if (!focused) {
        focus_client(clients);
        return;
    }
    Client *c = clients;
    while (c && c != focused) c = c->next;
    if (c && c->next) {
        focus_client(c->next);
    } else {
        focus_client(clients);
    }
}

static void focus_prev(void) {
    if (!clients) return;
    if (!focused) {
        focus_client(clients);
        return;
    }
    Client *prev = NULL;
    Client *c = clients;
    while (c && c != focused) {
        prev = c;
        c = c->next;
    }
    if (prev) {
        focus_client(prev);
    } else {
        Client *last = clients;
        while (last && last->next) last = last->next;
        if (last) focus_client(last);
    }
}

static void raise_focused(void) {
    if (!focused) return;
    XRaiseWindow(dpy, focused->win);
}

static void center_focused(void) {
    if (!focused) return;
    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    int nx = canvas_x + (sw - focused->w) / 2;
    int ny = canvas_y + (sh - focused->h) / 2;
    update_client_geometry(focused, nx, ny, focused->w, focused->h);
}

static void toggle_fullscreen_focused(void) {
    if (!focused) return;
    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    if (!focused->is_fullscreen) {
        focused->saved_x = focused->x;
        focused->saved_y = focused->y;
        focused->saved_w = focused->w;
        focused->saved_h = focused->h;
        focused->is_fullscreen = 1;
        update_client_geometry(focused, canvas_x, canvas_y, sw, sh);
    } else {
        focused->is_fullscreen = 0;
        update_client_geometry(focused,
                               focused->saved_x,
                               focused->saved_y,
                               focused->saved_w,
                               focused->saved_h);
    }
}

//binding system
static unsigned int parse_modifier_token(const char *tok) {
    if (strcmp(tok, "Shift") == 0)   return ShiftMask;
    if (strcmp(tok, "Control") == 0) return ControlMask;
    if (strcmp(tok, "Ctrl") == 0)    return ControlMask;
    if (strcmp(tok, "Alt") == 0)     return Mod1Mask;
    if (strcmp(tok, "Super") == 0)   return Mod4Mask;
    return 0;
}

static int parse_mouse_button(const char *tok) {
    if (strcmp(tok, "LMB") == 0) return Button1;
    if (strcmp(tok, "MMB") == 0) return Button2;
    if (strcmp(tok, "RMB") == 0) return Button3;
    return 0;
}

static void add_binding(Binding *b) {
    b->next = bindings;
    bindings = b;
}

static void free_bindings(void) {
    Binding *b = bindings;
    while (b) {
        Binding *next = b->next;
        if (b->cmd) free(b->cmd);
        free(b);
        b = next;
    }
    bindings = NULL;
}

static void add_default_mouse_binding(ActionType action, unsigned int mods, int button) {
    Binding *b = (Binding *)calloc(1, sizeof(Binding));
    if (!b) return;
    b->is_mouse = 1;
    b->modifiers = mods;
    b->button = button;
    b->action = action;
    add_binding(b);
}

static void add_default_key_binding(ActionType action, unsigned int mods, KeySym sym, const char *cmd) {
    Binding *b = (Binding *)calloc(1, sizeof(Binding));
    if (!b) return;
    b->is_mouse = 0;
    b->modifiers = mods;
    b->keycode = XKeysymToKeycode(dpy, sym);
    if (!b->keycode) {
        free(b);
        return;
    }
    b->action = action;
    if (action == ACTION_EXEC && cmd) {
        b->cmd = strdup(cmd);
    }
    add_binding(b);
}

static void load_default_bindings(void) {

    add_default_mouse_binding(ACTION_MOVEWINDOW, Mod1Mask, Button1);


    add_default_mouse_binding(ACTION_RESIZEWINDOW, Mod1Mask, Button3);


    add_default_mouse_binding(ACTION_MOVECANVAS, Mod1Mask | ControlMask, Button1);


    add_default_key_binding(ACTION_EXEC, Mod4Mask, XK_Return, "kitty");


    add_default_key_binding(ACTION_KILLWINDOW, Mod4Mask, XK_q, NULL);


    add_default_key_binding(ACTION_CLIPBOARD_SYNC, Mod4Mask, XK_c, NULL);
}

static void parse_bind_line(char *line) {

    char *comment = strstr(line, "//");
    if (comment) *comment = '\0';

    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "bind", 4) != 0) return;
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return;

    char *comma = strchr(p, ',');
    if (!comma) return;
    *comma = '\0';
    char *keyspec = p;
    char *rest = comma + 1;

    while (*rest == ' ' || *rest == '\t') rest++;
    if (strncmp(rest, "exec", 4) != 0) return;
    rest += 4;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (*rest != '=') return;
    rest++;
    while (*rest == ' ' || *rest == '\t') rest++;

    char *val_start = rest;
    char *semi = strchr(val_start, ';');
    if (!semi) return;
    *semi = '\0';

    char *val_end = val_start + strlen(val_start);
    while (val_end > val_start && (val_end[-1] == ' ' || val_end[-1] == '\t')) {
        *--val_end = '\0';
    }

    if (*val_start == '\'' || *val_start == '"') {
        char quote = *val_start;
        val_start++;
        char *q = strchr(val_start, quote);
        if (q) *q = '\0';
    }

    Binding *b = (Binding *)calloc(1, sizeof(Binding));
    if (!b) return;


    expand_vars(keyspec, strlen(keyspec) + 1);

    char *ks = strdup(keyspec);
    char *saveptr = NULL;
    char *tok = strtok_r(ks, "+", &saveptr);
    int is_mouse = 0;
    int button = 0;
    KeySym keysym = NoSymbol;

    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        unsigned int mod = parse_modifier_token(tok);
        if (mod) {
            b->modifiers |= mod;
        } else {
            int btn = parse_mouse_button(tok);
            if (btn) {
                is_mouse = 1;
                button = btn;
            } else {
                keysym = XStringToKeysym(tok);
            }
        }
        tok = strtok_r(NULL, "+", &saveptr);
    }
    free(ks);

    b->is_mouse = is_mouse;
    if (is_mouse) {
        b->button = button ? button : Button1;
    } else {
        if (keysym == NoSymbol) {
            free(b);
            return;
        }
        b->keycode = XKeysymToKeycode(dpy, keysym);
        if (!b->keycode) {
            free(b);
            return;
        }
    }

    if (strcmp(val_start, "MoveCanvas") == 0) {
        b->action = ACTION_MOVECANVAS;
    } else if (strcmp(val_start, "MoveWindow") == 0) {
        b->action = ACTION_MOVEWINDOW;
    } else if (strcmp(val_start, "ResizeWindow") == 0) {
        b->action = ACTION_RESIZEWINDOW;
    } else if (strcmp(val_start, "KillWindow") == 0) {
        b->action = ACTION_KILLWINDOW;
    } else if (strcmp(val_start, "FocusNext") == 0) {
        b->action = ACTION_FOCUSNEXT;
    } else if (strcmp(val_start, "FocusPrev") == 0) {
        b->action = ACTION_FOCUSPREV;
    } else if (strcmp(val_start, "RaiseWindow") == 0) {
        b->action = ACTION_RAISEWINDOW;
    } else if (strcmp(val_start, "CenterWindow") == 0) {
        b->action = ACTION_CENTERWINDOW;
    } else if (strcmp(val_start, "ToggleFullscreen") == 0) {
        b->action = ACTION_TOGGLEFULLSCREEN;
    } else if (strcmp(val_start, "ClipboardSync") == 0) {
        b->action = ACTION_CLIPBOARD_SYNC;
    } else {
        b->action = ACTION_EXEC;
        b->cmd = strdup(val_start);
    }

    add_binding(b);
}

static void ensure_core_bindings_present(void) {
    int have_move_window = 0;
    int have_resize_window = 0;
    int have_move_canvas = 0;
    int have_kill_window = 0;

    for (Binding *b = bindings; b; b = b->next) {
        switch (b->action) {
        case ACTION_MOVEWINDOW:      have_move_window = 1; break;
        case ACTION_RESIZEWINDOW:    have_resize_window = 1; break;
        case ACTION_MOVECANVAS:      have_move_canvas = 1; break;
        case ACTION_KILLWINDOW:      have_kill_window = 1; break;
        default: break;
        }
    }

    if (!have_move_window)
        add_default_mouse_binding(ACTION_MOVEWINDOW, Mod1Mask, Button1);
    if (!have_resize_window)
        add_default_mouse_binding(ACTION_RESIZEWINDOW, Mod1Mask, Button3);
    if (!have_move_canvas)
        add_default_mouse_binding(ACTION_MOVECANVAS, Mod1Mask | ControlMask, Button1);
    if (!have_kill_window)
        add_default_key_binding(ACTION_KILLWINDOW, Mod4Mask, XK_q, NULL);
}

static void load_config(void) {
    free_bindings();

    const char *home = getenv("HOME");
    if (!home || !*home) {
        load_default_bindings();
        return;
    }

    size_t len = strlen(home) + strlen(CONFIG_PATH_SUFFIX) + 1;
    char *path = (char *)malloc(len);
    if (!path) {
        load_default_bindings();
        return;
    }
    snprintf(path, len, "%s%s", home, CONFIG_PATH_SUFFIX);

    FILE *f = fopen(path, "r");
    free(path);

    if (!f) {
        load_default_bindings();
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&line, &cap, f)) != -1) {
        if (n <= 1) continue;


        char *p = line;
        while (*p == ' ' || *p == '\t') p++;


        if (*p == '#' || (*p == '/' && p[1] == '/')) continue;


        if (strncmp(p, "define", 6) == 0) {
            char name[128], value[128];
            if (sscanf(p, "define %127[^=]=%127s", name, value) == 2) {
                set_var(name, value);
            }
            continue;
        }


        if (strncmp(p, "autorun", 7) == 0) {
            char *eq = strchr(p, '=');
            if (eq) {
                eq++;
                while (*eq == ' ' || *eq == '\t') eq++;

                char *val = eq;


                char *end = val + strlen(val);
                while (end > val && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == ';'))
                    *--end = '\0';


                if (*val == '\'' || *val == '"') {
                    char quote = *val;
                    val++;
                    char *q = strchr(val, quote);
                    if (q) *q = '\0';
                }

                autorun_cmds = strdup(val);
            }
            continue;
        }


        parse_bind_line(p);
    }

    free(line);
    fclose(f);

    if (!bindings) {
        load_default_bindings();
    } else {
        ensure_core_bindings_present();
    }
}


static void run_autorun(void) {
    if (!autorun_cmds) return;

    char *cmds = strdup(autorun_cmds);
    char *saveptr = NULL;
    char *tok = strtok_r(cmds, ";", &saveptr);

    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok) spawn(tok);
        tok = strtok_r(NULL, ";", &saveptr);
    }

    free(cmds);
}

static void grab_keys(void) {
    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    for (Binding *b = bindings; b; b = b->next) {
        if (!b->is_mouse && b->keycode) {
            XGrabKey(dpy, b->keycode, b->modifiers, root, True,
                     GrabModeAsync, GrabModeAsync);
        }
    }
}



static unsigned int clean_mods(unsigned int state) {
    unsigned int mask = ShiftMask | ControlMask | Mod1Mask | Mod4Mask;
    return state & mask;
}

static Binding *find_key_binding(unsigned int state, KeyCode code) {
    unsigned int mods = clean_mods(state);
    for (Binding *b = bindings; b; b = b->next) {
        if (!b->is_mouse && b->keycode == (int)code && b->modifiers == mods) {
            return b;
        }
    }
    return NULL;
}

static Binding *find_mouse_binding(unsigned int state, unsigned int button) {
    unsigned int mods = clean_mods(state);
    for (Binding *b = bindings; b; b = b->next) {
        if (b->is_mouse && b->button == (int)button && b->modifiers == mods) {
            return b;
        }
    }
    return NULL;
}



static Client *client_at_position(int rx, int ry) {
    for (Client *c = clients; c; c = c->next) {
        int sx = c->x - canvas_x;
        int sy = c->y - canvas_y;
        if (rx >= sx && rx < sx + c->w &&
            ry >= sy && ry < sy + c->h) {
            return c;
        }
    }
    return NULL;
}



static void handle_keypress(XKeyEvent *ev) {
    Binding *b = find_key_binding(ev->state, ev->keycode);
    if (!b) return;

    switch (b->action) {
    case ACTION_EXEC:
        if (b->cmd) spawn(b->cmd);
        break;
    case ACTION_MOVECANVAS:
    case ACTION_MOVEWINDOW:
    case ACTION_RESIZEWINDOW:
        break;
    case ACTION_KILLWINDOW:
        kill_focused();
        break;
    case ACTION_FOCUSNEXT:
        focus_next();
        break;
    case ACTION_FOCUSPREV:
        focus_prev();
        break;
    case ACTION_RAISEWINDOW:
        raise_focused();
        break;
    case ACTION_CENTERWINDOW:
        center_focused();
        break;
    case ACTION_TOGGLEFULLSCREEN:
        toggle_fullscreen_focused();
        break;
    case ACTION_CLIPBOARD_SYNC:
        clipboard_sync();
        break;
    }
}

static void handle_button_press(XButtonEvent *ev) {
    Binding *b = find_mouse_binding(ev->state, ev->button);

    if (!b) {
        Client *c = client_at_position(ev->x_root, ev->y_root);
        if (c) focus_client(c);
        return;
    }

    drag_start_root_x = ev->x_root;
    drag_start_root_y = ev->y_root;
    drag_start_canvas_x = canvas_x;
    drag_start_canvas_y = canvas_y;
    drag_client = NULL;
    drag_client_start_x = drag_client_start_y = 0;
    drag_client_start_w = drag_client_start_h = 0;

    switch (b->action) {
    case ACTION_MOVECANVAS:
        drag_mode = DRAG_CANVAS;
        break;
    case ACTION_MOVEWINDOW: {
        Client *c = client_at_position(ev->x_root, ev->y_root);
        if (c) {
            drag_client = c;
            drag_client_start_x = c->x;
            drag_client_start_y = c->y;
            drag_mode = DRAG_WINDOW_MOVE;
            focus_client(c);
        } else {
            drag_mode = DRAG_NONE;
        }
        break;
    }
    case ACTION_RESIZEWINDOW: {
        Client *c = client_at_position(ev->x_root, ev->y_root);
        if (c) {
            drag_client = c;
            drag_client_start_x = c->x;
            drag_client_start_y = c->y;
            drag_client_start_w = c->w;
            drag_client_start_h = c->h;
            drag_mode = DRAG_WINDOW_RESIZE;
            focus_client(c);
        } else {
            drag_mode = DRAG_NONE;
        }
        break;
    }
    case ACTION_EXEC:
        if (b->cmd) spawn(b->cmd);
        drag_mode = DRAG_NONE;
        break;
    case ACTION_KILLWINDOW:
        kill_focused();
        drag_mode = DRAG_NONE;
        break;
    case ACTION_FOCUSNEXT:
        focus_next();
        drag_mode = DRAG_NONE;
        break;
    case ACTION_FOCUSPREV:
        focus_prev();
        drag_mode = DRAG_NONE;
        break;
    case ACTION_RAISEWINDOW:
        raise_focused();
        drag_mode = DRAG_NONE;
        break;
    case ACTION_CENTERWINDOW:
        center_focused();
        drag_mode = DRAG_NONE;
        break;
    case ACTION_TOGGLEFULLSCREEN:
        toggle_fullscreen_focused();
        drag_mode = DRAG_NONE;
        break;
    case ACTION_CLIPBOARD_SYNC:
        clipboard_sync();
        drag_mode = DRAG_NONE;
        break;
    }
}

static void handle_button_release(XButtonEvent *ev) {
    (void)ev;
    drag_mode = DRAG_NONE;
    drag_client = NULL;
}

static void handle_motion_notify(XMotionEvent *ev) {
    if (drag_mode == DRAG_NONE) return;

    int dx = ev->x_root - drag_start_root_x;
    int dy = ev->y_root - drag_start_root_y;

    switch (drag_mode) {
    case DRAG_CANVAS:
        canvas_x = drag_start_canvas_x - dx;
        canvas_y = drag_start_canvas_y - dy;
        reposition_all_clients();
        break;
    case DRAG_WINDOW_MOVE:
        if (drag_client) {
            int nx = drag_client_start_x + dx;
            int ny = drag_client_start_y + dy;
            update_client_geometry(drag_client, nx, ny, drag_client->w, drag_client->h);
            restack_special_clients();
        }
        break;
    case DRAG_WINDOW_RESIZE:
        if (drag_client) {
            int nw = drag_client_start_w + dx;
            int nh = drag_client_start_h + dy;
            if (nw < 50) nw = 50;
            if (nh < 50) nh = 50;
            update_client_geometry(drag_client,
                                   drag_client_start_x,
                                   drag_client_start_y,
                                   nw, nh);
            restack_special_clients();
        }
        break;
    default:
        break;
    }
}

static void handle_configure_request(XConfigureRequestEvent *e) {
    Client *c = find_client(e->window);
    XWindowChanges wc;
    wc.x = e->x;
    wc.y = e->y;
    wc.width = e->width;
    wc.height = e->height;
    wc.border_width = e->border_width;
    wc.sibling = e->above;
    wc.stack_mode = e->detail;

    XConfigureWindow(dpy, e->window, e->value_mask, &wc);

    if (c) {
        int lx = wc.x + canvas_x;
        int ly = wc.y + canvas_y;
        int lw = wc.width;
        int lh = wc.height;
        update_client_geometry(c, lx, ly, lw, lh);
        restack_special_clients();
    }
}

static void handle_map_request(XMapRequestEvent *e) {
    manage_window(e->window);
}

static void handle_destroy_notify(XDestroyWindowEvent *e) {
    unmanage_window(e->window);
}

static void handle_unmap_notify(XUnmapEvent *e) {
    if (e->event != root) {
        unmanage_window(e->window);
    }
}

static void handle_enter_notify(XCrossingEvent *ev) {
    if (ev->mode != NotifyNormal && ev->mode != NotifyUngrab) return;
    Client *c = find_client(ev->window);
    if (c) focus_client(c);
}

//main
int main(void) {
    setenv("XDG_CURRENT_DESKTOP", "SukaWM", 1);
    setenv("XDG_SESSION_DESKTOP", "SukaWM", 1);
    setenv("XDG_SESSION_TYPE", "x11", 1);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "sukawm: cannot open display\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    XSetErrorHandler(xerror_handler);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    XSelectInput(dpy, root,
                 SubstructureRedirectMask |
                 SubstructureNotifyMask |
                 ButtonPressMask |
                 ButtonReleaseMask |
                 PointerMotionMask |
                 KeyPressMask);

    wm_protocols     = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    load_config();
    grab_keys();
    set_background();
    run_autorun();

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case MapRequest:
            handle_map_request(&ev.xmaprequest);
            break;
        case ConfigureRequest:
            handle_configure_request(&ev.xconfigurerequest);
            break;
        case DestroyNotify:
            handle_destroy_notify(&ev.xdestroywindow);
            break;
        case UnmapNotify:
            handle_unmap_notify(&ev.xunmap);
            break;
        case KeyPress:
            handle_keypress(&ev.xkey);
            break;
        case ButtonPress:
            handle_button_press(&ev.xbutton);
            break;
        case ButtonRelease:
            handle_button_release(&ev.xbutton);
            break;
        case MotionNotify:
            handle_motion_notify(&ev.xmotion);
            break;
        case EnterNotify:
            handle_enter_notify(&ev.xcrossing);
            break;
        default:
            break;
        }
    }

    XCloseDisplay(dpy);
    return 0;
}

