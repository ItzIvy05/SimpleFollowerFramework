<h1 align="center"><u>Simple Follower Framework</u></h1>

Simple Follower Framework is a SKSE follower framework that allows you to have upto 8 vanilla followers. This mod does not add a custom follower system. It does not replace the vanilla follower quest. It just extends the limit and keeps vanilla behavior.

<font color="#00ff00"><b>This mod does not touch any modded followers with their own system (Inigo, Lucien, Remiel, etc) and never will. That is out of this mod's scope. So this mod will not conflict or touch those mods.</b></font>

## What this mod does
- Lets you recruit more than one vanilla follower.
- The max follower cap can be adjusted in **"SimpleFollowerFramework.ini"** by editing **"iMaxFollowers."**
- **Perk Based Follower Locking**: You can make each owned perk unlock one extra follower slot, up to 8 followers.
- **Essential Followers:** Flags the followers that are in the **'CurrentFollowerFaction'** as Essential."

## Why this mod is a thing:
I wanted a lightweight follower framework that handled the basics without being buggy or bloating the Papyrus VM. I couldn't find one, so I made my own using SKSE.

## Requirements
- [Skyrim Script Extender (SKSE64)](https://www.nexusmods.com/skyrimspecialedition/mods/30379)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

**Works out of the box with**
- [Unofficial Skyrim Special Edition Patch](https://www.nexusmods.com/skyrimspecialedition/mods/266)
- Any modded followers that uses Vanilla Follower system will be able to take advantage of this mod.

**Not Compatible with:**
- [Nether's Follower Framework](https://www.nexusmods.com/skyrimspecialedition/mods/55653)
- [Busy Follower Framework](https://www.nexusmods.com/skyrimspecialedition/mods/112076)
- [Amazing Follower Tweaks SE](https://www.nexusmods.com/skyrimspecialedition/mods/6656)
- Or any Follower Frameworks

## Configuration

**"iMaxFollowers":** This is your total follower cap, including the normal vanilla slot. Up-to 8. For Example:
- `iMaxFollowers=1 - ` Vanilla behavior (one follower).
- `iMaxFollowers=4 - ` You can have up to four vanilla followers at once.

**"bFollowerOptionSelector":** Follower Perk Option Selector
- `bFollowerOptionSelecto=0 - ` Option 1: Feature is turned off, and it will use iMaxFollowers.
- `bFollowerOptionSelecto=1 - ` Option 2: Use perk list to add follower slots (ignores iMaxFollowers)

**"sPerkForm":** This is only used when **"bFollowerOptionSelector"**=1
- Comma-separated list, up to 8 entries
- Perk Format: PluginName|LocalFormID
- sPerkForms=Skyrim.esm|00058F75,Skyrim.esm|00058f7a,Skyrim.esm|00058f7b [Example]
- They can be any perk you want. It can be a new perk or any vanilla perk
- Each perk will open an extra slot for a follower.

**"bFollowerEssential":** If enabled, any loaded actor that is in "CurrentFollowerFaction" gets their base flagged Essential.  
That means your current vanilla followers you have with you will stop dying.

## Check out my other mods
[![ ](https://i.imgur.com/nzFC1ji.png)](https://next.nexusmods.com/profile/ItzIvy/mods?gameId=1704)

![ ](https://i.imgur.com/HeFJ1bb.png)

**Join Discord to interact with the community, answer support inquiries, and more!**

[![ ](https://i.imgur.com/RtdOG3V.png)](https://discord.gg/FB62v6whbh)
