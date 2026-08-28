<h1 align=center>Genshin Impact NX</h1>

A Non working wrapper/port of the Android release of **Genshin Impact** (package
`com.miHoYo.GenshinImpact`, version `6.7.0`, versionCode 1206). It loads the
original game binary `libyuanshen.so` (Unity 2017.4 / IL2CPP), applies Android
APS2 packed relocations and runs it inside a minimal Android-like environment
natively on the Switch.


This is not a playable port, the wrapper carries the client all the way through account login and the full game-data download, but Genshin Impact's anti-cheat then blocks it from running.

It is published as a base for other Android Unity ports.