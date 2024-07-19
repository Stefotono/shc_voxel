# Sky High Cats - Voxels
============================

a fork of [godot_voxels](https://github.com/Zylann/godot_voxel/tree/ba4f59fbf74c8c68e177b16209531048533adad5) which goes ahead and just implements the nerd physics related junk needed for shc.

## Main features for fork (planned)
- Physics Terrain (Voxel terrain but now it`s a rigid body and all of it needs to be loaded.)
- i dunno anything much else, get back to me on this one.

Features
---------------------------

- Realtime 3D terrain editable in-game (Unlike a heightmap based terrain, this allows for overhangs, tunnels, and user creation/destruction)
- Polygon-based: voxels are transformed into chunked meshes to be rendered
- Godot physics integration + alternate fast Minecraft-like collisions
- Infinite terrains made by paging chunks in and out
- Voxel data is streamed from a variety of sources, which includes the ability to write your own generators
- Minecraft-style blocky voxel terrain, with multiple materials and baked ambient occlusion
- Smooth terrain with level of detail using Transvoxel
- Voxel storage using 8-bit or 16-bit channels for any general purpose
- Instancing system to spawn foliage, rocks and other decoration on surfaces
