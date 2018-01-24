Butler
======

A really simple Jenkins build notification app for Windows. Keep an eye on your builds from the system tray!
![Main Window](https://github.com/zanders3/butler/blob/master/mainwindow.png "Main Window")

Setup
=====

Either [Download](https://github.com/zanders3/butler/releases) and run the latest exe or open `butler.sln` in Visual Studio 2015 and press F5.

The settings screen will then ask for the HTTP path to your Jenkin's cc.xml e.g. `http://jenkins/cc.xml`. Settings are stored in a `settings.ini` alongside the exe.

![Settings](https://github.com/zanders3/butler/blob/master/settings.png "Settings")

The focus for this app is on minimal use of resources - the exe is 1.1MB and eats a whole 1.7MB of memory when running!
