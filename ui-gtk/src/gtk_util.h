#pragma once

#include <adwaita.h>

#include <functional>
#include <string>

// Small helpers for using the GTK C API from C++ without gtkmm.
namespace cardinal::gtk {

using Fn = std::function<void()>;

inline void destroyFn(gpointer data, GClosure*) { delete static_cast<Fn*>(data); }

// Signals shaped (instance, user_data): "clicked", "activate", "changed", "closed", ...
inline gulong on(gpointer instance, const char* signal, Fn fn) {
    return g_signal_connect_data(
        instance, signal,
        G_CALLBACK(+[](gpointer, gpointer data) { (*static_cast<Fn*>(data))(); }),
        new Fn(std::move(fn)), destroyFn, G_CONNECT_DEFAULT);
}

// "notify::property": (instance, GParamSpec*, user_data)
inline gulong onNotify(gpointer instance, const char* signal, Fn fn) {
    return g_signal_connect_data(
        instance, signal,
        G_CALLBACK(+[](gpointer, GParamSpec*, gpointer data) { (*static_cast<Fn*>(data))(); }),
        new Fn(std::move(fn)), destroyFn, G_CONNECT_DEFAULT);
}

inline std::string str(const char* s) { return s ? s : ""; }

inline void addClass(GtkWidget* w, const char* cls) { gtk_widget_add_css_class(w, cls); }

inline GtkWidget* label(const std::string& text, const char* cssClass = nullptr, float xalign = 0) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    if (cssClass) addClass(l, cssClass);
    return l;
}

inline void clearChildren(GtkWidget* box) {
    while (GtkWidget* c = gtk_widget_get_first_child(box)) gtk_box_remove(GTK_BOX(box), c);
}

// Trello label color names -> CSS classes defined in style.css
inline std::string labelClass(const std::string& color) {
    return "lbl-" + (color.empty() ? std::string("none") : color);
}

}  // namespace cardinal::gtk
