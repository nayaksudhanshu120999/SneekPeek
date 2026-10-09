#pragma once
// SneekPeek - Windows Settings pages: name + keywords + ms-settings: URI.
// Static table (~10KB), matched with the same fuzzy scorer as apps.
// URIs are the long-stable set; a few may vary across Windows builds.
#include <string>
#include <vector>

struct SysPage {
    const wchar_t* name;
    const wchar_t* keywords;
    const wchar_t* uri;
};

inline const SysPage kSysPages[] = {
    {L"Bluetooth & devices", L"bluetooth devices pair", L"ms-settings:bluetooth"},
    {L"Printers & scanners", L"printers scanners print", L"ms-settings:printers"},
    {L"Mouse", L"mouse touchpad pointer cursor", L"ms-settings:mousetouchpad"},
    {L"Display", L"display monitor brightness resolution night light", L"ms-settings:display"},
    {L"Sound", L"sound volume audio speakers microphone", L"ms-settings:sound"},
    {L"Notifications", L"notifications alerts banners", L"ms-settings:notifications"},
    {L"Power & battery", L"power battery sleep saver", L"ms-settings:powersleep"},
    {L"Storage", L"storage disk space cleanup", L"ms-settings:storagesense"},
    {L"Multitasking", L"multitasking snap windows virtual desktop", L"ms-settings:multitasking"},
    {L"Clipboard", L"clipboard copy paste history", L"ms-settings:clipboard"},
    {L"Nearby sharing", L"nearby sharing send receive", L"ms-settings:crossdevice"},
    {L"USB", L"usb devices connection", L"ms-settings:usb"},
    {L"AutoPlay", L"autoplay removable defaults", L"ms-settings:autoplay"},
    {L"Network & internet", L"network internet connection status", L"ms-settings:network"},
    {L"Wi-Fi", L"wifi wireless network", L"ms-settings:network-wifi"},
    {L"Ethernet", L"ethernet wired network", L"ms-settings:network-ethernet"},
    {L"VPN", L"vpn virtual private network", L"ms-settings:network-vpn"},
    {L"Proxy", L"proxy server network", L"ms-settings:network-proxy"},
    {L"Mobile hotspot", L"hotspot share tether", L"ms-settings:network-mobilehotspot"},
    {L"Data usage", L"data usage metered limit", L"ms-settings:datausage"},
    {L"Windows Update", L"update upgrade check install restart", L"ms-settings:windowsupdate"},
    {L"Delivery Optimization", L"delivery optimization download bandwidth", L"ms-settings:delivery-optimization"},
    {L"Windows Security", L"security antivirus defender firewall threat", L"ms-settings:windowsdefender"},
    {L"Backup", L"backup restore onedrive recovery", L"ms-settings:backup"},
    {L"Recovery", L"recovery reset reset restore advanced startup", L"ms-settings:recovery"},
    {L"Activation", L"activation license product key genuine", L"ms-settings:activation"},
    {L"Accounts", L"accounts user your info email", L"ms-settings:yourinfo"},
    {L"Sign-in options", L"sign pin password hello fingerprint face", L"ms-settings:signinoptions"},
    {L"Sync your settings", L"sync settings backup account", L"ms-settings:sync"},
    {L"Date & time", L"date time clock timezone", L"ms-settings:dateandtime"},
    {L"Language & region", L"language region keyboard locale", L"ms-settings:regionlanguage"},
    {L"Speech", L"speech voice recognition cortana", L"ms-settings:speech"},
    {L"Apps & features", L"apps features installed uninstall programs", L"ms-settings:appsfeatures"},
    {L"Default apps", L"default apps open with browser", L"ms-settings:defaultapps"},
    {L"Startup apps", L"startup apps boot launch background", L"ms-settings:startupapps"},
    {L"Optional features", L"optional features add remove", L"ms-settings:optionalfeatures"},
    {L"Offline maps", L"offline maps download", L"ms-settings:maps"},
    {L"Apps for websites", L"apps websites links open", L"ms-settings:appsforwebsites"},
    {L"Video playback", L"video playback hdr battery", L"ms-settings:videoplayback"},
    {L"Background", L"background wallpaper picture slideshow", L"ms-settings:personalization-background"},
    {L"Colors", L"colors theme dark light accent transparency", L"ms-settings:colors"},
    {L"Lock screen", L"lock screen picture slideshow timeout", L"ms-settings:lockscreen"},
    {L"Themes", L"themes contrast background sound", L"ms-settings:themes"},
    {L"Fonts", L"fonts install type", L"ms-settings:fonts"},
    {L"Taskbar", L"taskbar pin icons hide search widgets", L"ms-settings:taskbar"},
    {L"Start", L"start menu folders layout pins", L"ms-settings:personalization-start"},
    {L"Game Bar", L"game bar record clips xbox", L"ms-settings:gaming-gamebar"},
    {L"Captures", L"captures record dvr clips background", L"ms-settings:gaming-gamedvr"},
    {L"Game Mode", L"game mode performance", L"ms-settings:gaming-gamemode"},
    {L"Ease of Access", L"ease access narrator magnifier ease", L"ms-settings:easeofaccess-narrator"},
    {L"Magnifier", L"magnifier zoom ease access", L"ms-settings:easeofaccess-magnifier"},
    {L"High contrast", L"high contrast theme ease access", L"ms-settings:easeofaccess-highcontrast"},
    {L"Closed captions", L"closed captions subtitles", L"ms-settings:easeofaccess-closedcaptioning"},
    {L"Keyboard", L"keyboard sticky filter ease access", L"ms-settings:easeofaccess-keyboard"},
    {L"Privacy & security", L"privacy security location camera microphone", L"ms-settings:privacy"},
    {L"Location", L"location gps privacy", L"ms-settings:privacy-location"},
    {L"Camera privacy", L"camera privacy webcam", L"ms-settings:privacy-webcam"},
    {L"Microphone privacy", L"microphone privacy mic", L"ms-settings:privacy-microphone"},
    {L"Background apps", L"background apps privacy battery", L"ms-settings:privacy-backgroundapps"},
    {L"About", L"about system specs device rename", L"ms-settings:about"},
};

inline int SysPageCount() {
    return (int)(sizeof(kSysPages) / sizeof(kSysPages[0]));
}
