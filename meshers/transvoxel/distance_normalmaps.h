#ifndef VOXEL_DISTANCE_NORMALMAPS_H
#define VOXEL_DISTANCE_NORMALMAPS_H

#include "../../util/math/vector3f.h"
#include "transvoxel.h"

#include <core/object/ref_counted.h>
#include <vector>

class Texture2DArray;
class Texture2D;

namespace zylann::voxel {

class VoxelGenerator;

// UV-mapping a voxel mesh is not trivial, but if mapping is required, an alternative is to subdivide the mesh into a
// grid of cells (we can use Transvoxel cells). In each cell, pick an axis-aligned projection working best with
// triangles of the cell using the average of their normals. A tile can then be generated by projecting its pixels on
// triangles, and be stored in an atlas. A shader can then read the atlas using a lookup texture to find the tile.

struct NormalMapData {
	struct EncodedNormal {
		uint8_t x;
		uint8_t y;
		uint8_t z;
	};
	// Encoded normals
	std::vector<EncodedNormal> normals;
	struct Tile {
		uint8_t x;
		uint8_t y;
		uint8_t z;
		uint8_t axis;
	};
	std::vector<Tile> tiles;

	inline void clear() {
		normals.clear();
		tiles.clear();
	}
};

// For each non-empty cell of the mesh, choose an axis-aligned projection based on triangle normals in the cell.
// Sample voxels inside the cell to compute a tile of world space normals from the SDF.
void compute_normalmap(Span<const transvoxel::CellInfo> cell_infos, const transvoxel::MeshArrays &mesh,
		NormalMapData &normal_map_data, unsigned int tile_resolution, VoxelGenerator &generator,
		Vector3i origin_in_voxels, unsigned int lod_index);

// struct NormalMapImages {
// 	Vector<Ref<Image>> atlas_images;
// 	Ref<Image> lookup_image;
// };

struct NormalMapTextures {
	Ref<Texture2DArray> atlas;
	Ref<Texture2D> lookup;
};

// Converts normalmap data into textures. They can be used in a shader to apply normals and obtain extra visual details.
NormalMapTextures store_normalmap_data_to_textures(
		const NormalMapData &data, unsigned int tile_resolution, Vector3i block_size);

} // namespace zylann::voxel

#endif // VOXEL_DISTANCE_NORMALMAPS_H
