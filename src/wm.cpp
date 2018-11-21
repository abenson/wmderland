#include "wm.hpp"
#include "global.hpp"
#include "client.hpp"
#include "util.hpp"
#include <string>
#include <algorithm>
#include <X11/cursorfont.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include <glog/logging.h>

WindowManager* WindowManager::instance_;

WindowManager* WindowManager::GetInstance() {
    // If the instance is not yet initialized, we'll try to open a display
    // to X server. If it fails (i.e., dpy is None), then we return None
    // to the caller. Otherwise we return an instance of WindowManager.
    if (!instance_) {
        Display* dpy;
        instance_ = (dpy = XOpenDisplay(None)) ? new WindowManager(dpy) : None;
    }
    return instance_;
}

WindowManager::WindowManager(Display* dpy) {
    dpy_ = dpy;
    current_ = 0;
    fullscreen_ = false;

    // Initialize property manager and set _NET_WM_NAME.
    property_mgr_ = new PropertyManager(dpy_);
    property_mgr_->Set(
            DefaultRootWindow(dpy_), 
            property_mgr_->GetNetAtom(PropertyManager::NET_WM_NAME),
            property_mgr_->utf8string(),
            8, PropModeReplace, (unsigned char*) WM_NAME, sizeof(WM_NAME)
    );

    // Initialize 10 workspaces.
    for (int i = 0; i < WORKSPACE_COUNT - 1; i++) {
        workspaces_.push_back(new Workspace(dpy_, i));
    }

    // Initializes cursor.
    cursors_[LEFT_PTR_CURSOR] = XCreateFontCursor(dpy_, XC_left_ptr);
    cursors_[RESIZE_CURSOR] = XCreateFontCursor(dpy_, XC_sizing);
    cursors_[MOVE_CURSOR] = XCreateFontCursor(dpy_, XC_fleur);
    SetCursor(DefaultRootWindow(dpy_), cursors_[LEFT_PTR_CURSOR]);

    // Define which key combinations will send us X events.
    XGrabKey(dpy_, AnyKey, Mod4Mask, DefaultRootWindow(dpy_), True, GrabModeAsync, GrabModeAsync);

    // Define which mouse clicks will send us X events.
    XGrabButton(dpy_, AnyButton, AnyModifier, DefaultRootWindow(dpy_), True,
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);

    // Enable substructure redirection on the root window.
    XSelectInput(dpy_, DefaultRootWindow(dpy_), SubstructureNotifyMask | SubstructureRedirectMask);

    // Setup the bitch catcher.
    XSetErrorHandler(&WindowManager::OnXError);
}

WindowManager::~WindowManager() {
    for (auto w : workspaces_) {
        delete w;
    }

    delete property_mgr_;
    XCloseDisplay(dpy_);
}


void WindowManager::Run() {
    system("displayctl && ~/.config/polybar/launch.sh");

    for(;;) {
        // Retrieve and dispatch next X event.
        XNextEvent(dpy_, &event_);

        switch (event_.type) {
            case CreateNotify:
                OnCreateNotify();
                break;
            case DestroyNotify:
                OnDestroyNotify();
                break;
            case MapRequest:
                OnMapRequest();
                break;
            case KeyPress:
                OnKeyPress();
                break;
            case ButtonPress:
                OnButtonPress();
                break;
            case ButtonRelease:
                OnButtonRelease();
                break;
            case MotionNotify:
                OnMotionNotify();
                break;
            case FocusIn:
                OnFocusIn();
                break;
            case FocusOut:
                OnFocusOut();
                break;
            default:
                break;
        }
    }
}

void WindowManager::OnCreateNotify() {

}

void WindowManager::OnDestroyNotify() {
    // When a window is destroyed, remove it from the current workspace's client list.
    workspaces_[current_]->Remove(event_.xdestroywindow.window);
}

void WindowManager::OnMapRequest() {
    // Just map the window now. We'll discuss other things later.
    Window w = event_.xmaprequest.window;
    XMapWindow(dpy_, w);

    // Bars should not have border or be added to a workspace.
    // We check if w is a bar by inspecting its WM_CLASS.
    if (wm_utils::IsBar(dpy_, w)) {
        return;
    }
    
    // Regular applications should be added to workspace client list,
    // but first we have to check if it's already in the list!
    if (!workspaces_[current_]->Has(w)) {
        // XSelectInput() and Borders are automatically done 
        // in the constructor of Client class.
        workspaces_[current_]->Add(new Client(dpy_, w));
        Center(w);
    }

    // Set the newly mapped client as the focused one.
    workspaces_[current_]->SetFocusClient(w);
}

void WindowManager::OnKeyPress() {
    // Key pressed but does NOT require any window to be focused.
    // Mod4 + Return -> Spawn urxvt.
    if (event_.xkey.keycode == XKeysymToKeycode(dpy_, XStringToKeysym("Return"))) {
        system("urxvt &");
        return;
    } else if (event_.xkey.keycode == XKeysymToKeycode(dpy_, XStringToKeysym("d"))) {
        system("rofi -show drun");
        return;
    } else if (event_.xkey.keycode >= XKeysymToKeycode(dpy_, XStringToKeysym("1"))
            && event_.xkey.keycode <= XKeysymToKeycode(dpy_, XStringToKeysym("9"))) {
        GotoWorkspace(event_.xkey.keycode - 10);
        return;
    }

    if (event_.xkey.subwindow == None) {
        return;
    }


    // Mod4 + q -> Kill window.
    if (event_.xkey.keycode == XKeysymToKeycode(dpy_, XStringToKeysym("q"))) {
        XKillClient(dpy_, event_.xkey.subwindow);
    } else if (event_.xkey.keycode == XKeysymToKeycode(dpy_, XStringToKeysym("f"))) {
        XRaiseWindow(dpy_, event_.xkey.subwindow);

        if (!fullscreen_) {
            // Record the current window's position and size before making it fullscreen.
            XGetWindowAttributes(dpy_, event_.xkey.subwindow, &attr_);
            XMoveResizeWindow(dpy_, event_.xkey.subwindow, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
            fullscreen_ = true;
        } else {
            // Restore the window to its original position and size.
            XMoveResizeWindow(dpy_, event_.xkey.subwindow, attr_.x, attr_.y, attr_.width, attr_.height);
            fullscreen_ = false;
        }
    }
}

void WindowManager::OnButtonPress() {
    if (event_.xbutton.subwindow == None) {
        return;
    }

    // Clicking on a window raises that window to the top.
    XRaiseWindow(dpy_, event_.xbutton.subwindow);
    XSetInputFocus(dpy_, event_.xbutton.subwindow, RevertToParent, CurrentTime);
    workspaces_[current_]->SetFocusClient(event_.xbutton.subwindow);

    if (event_.xbutton.state == Mod4Mask) {
        // Lookup the attributes (e.g., size and position) of a window
        // and store the result in attr_
        XGetWindowAttributes(dpy_, event_.xbutton.subwindow, &attr_);
        start_ = event_.xbutton;

        SetCursor(DefaultRootWindow(dpy_), cursors_[event_.xbutton.button]);
    }
}

void WindowManager::OnButtonRelease() {
    start_.subwindow = None;
    SetCursor(DefaultRootWindow(dpy_), cursors_[LEFT_PTR_CURSOR]);
}

void WindowManager::OnMotionNotify() {
    if (start_.subwindow == None) return;

    // Dragging a window around also raises it to the top.
    int xdiff = event_.xbutton.x - start_.x;
    int ydiff = event_.xbutton.y - start_.y;

    int new_x = attr_.x + ((start_.button == MOUSE_LEFT_BTN) ? xdiff : 0);
    int new_y = attr_.y + ((start_.button == MOUSE_LEFT_BTN) ? ydiff : 0);
    int new_width = attr_.width + ((start_.button == MOUSE_RIGHT_BTN) ? xdiff : 0);
    int new_height = attr_.height + ((start_.button == MOUSE_RIGHT_BTN) ? ydiff : 0);

    if (new_width < MIN_WINDOW_WIDTH) new_width = MIN_WINDOW_WIDTH;
    if (new_height < MIN_WINDOW_HEIGHT) new_height = MIN_WINDOW_HEIGHT;
    XMoveResizeWindow(dpy_, start_.subwindow, new_x, new_y, new_width, new_height);
}

void WindowManager::OnFocusIn() {
    Window w = event_.xfocus.window;
    XSetWindowBorder(dpy_, w, FOCUSED_COLOR);

    std::string wm_class = wm_utils::QueryWmClass(dpy_, w);

    XChangeProperty(dpy_, DefaultRootWindow(dpy_),
            XInternAtom(dpy_, "_NET_ACTIVE_WINDOW", False),
            XInternAtom(dpy_, "UTF8_STRING", False),
            8, PropModeReplace, (unsigned char *) wm_class.c_str(), wm_class.length());
}

void WindowManager::OnFocusOut() {
    Window w = event_.xfocus.window;
    XSetWindowBorder(dpy_, w, UNFOCUSED_COLOR);

    std::string wm_class = wm_utils::QueryWmClass(dpy_, w);

//    XDeleteProperty(dpy_, w, "_NET_ACTIVE_WINDOW");
}

int WindowManager::OnXError(Display* dpy, XErrorEvent* e) {
    const int MAX_ERROR_TEXT_LENGTH = 1024;
    char error_text[MAX_ERROR_TEXT_LENGTH];
    XGetErrorText(dpy, e->error_code, error_text, sizeof(error_text));
    LOG(ERROR) << "Received X error:\n"
        << "    Request: " << int(e->request_code)
        << "    Error code: " << int(e->error_code)
        << " - " << error_text << "\n"
        << "    Resource ID: " << e->resourceid;
    // The return value is ignored.
    return 0;
}

void WindowManager::SetCursor(Window w, Cursor c) {
    XDefineCursor(dpy_, w, c);
}

void WindowManager::GotoWorkspace(short next) {
    if (current_ == next) return;
    
    workspaces_[current_]->UnmapAllClients();
    workspaces_[next]->MapAllClients();
    current_ = next;
}



void WindowManager::Center(Window w) {
    XWindowAttributes w_attr = wm_utils::QueryWindowAttributes(dpy_, w);
    int new_x = SCREEN_WIDTH / 2 - w_attr.width / 2;
    int new_y = SCREEN_HEIGHT / 2 - w_attr.height / 2;
    XMoveWindow(dpy_, w, new_x, new_y);
}
