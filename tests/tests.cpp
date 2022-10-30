#include "tests.h"
#include "../edition/voxel_tool_terrain.h"
#include "../generators/graph/range_utility.h"
#include "../generators/graph/voxel_generator_graph.h"
#include "../generators/graph/voxel_graph_node_db.h"
#include "../meshers/blocky/voxel_blocky_library.h"
#include "../meshers/cubes/voxel_mesher_cubes.h"
#include "../storage/voxel_buffer_gd.h"
#include "../storage/voxel_data.h"
#include "../storage/voxel_data_map.h"
#include "../storage/voxel_metadata_variant.h"
#include "../streams/instance_data.h"
#include "../streams/region/region_file.h"
#include "../streams/region/voxel_stream_region_files.h"
#include "../streams/voxel_block_serializer.h"
#include "../streams/voxel_block_serializer_gd.h"
#include "../util/container_funcs.h"
#include "../util/expression_parser.h"
#include "../util/flat_map.h"
#include "../util/godot/funcs.h"
#include "../util/island_finder.h"
#include "../util/math/box3i.h"
#include "../util/noise/fast_noise_lite/fast_noise_lite.h"
#include "../util/string_funcs.h"
#include "../util/tasks/threaded_task_runner.h"
#include "test_octree.h"
#include "testing.h"

#ifdef VOXEL_ENABLE_FAST_NOISE_2
#include "../util/noise/fast_noise_2.h"
#endif

#include <core/io/dir_access.h>
#include <core/io/stream_peer.h>
#include <core/string/print_string.h>
#include <core/templates/hash_map.h>
#include <modules/noise/fastnoise_lite.h>

namespace zylann::voxel::tests {

void test_box3i_intersects() {
	{
		Box3i a(Vector3i(0, 0, 0), Vector3i(1, 1, 1));
		Box3i b(Vector3i(0, 0, 0), Vector3i(1, 1, 1));
		ZN_TEST_ASSERT(a.intersects(b));
	}
	{
		Box3i a(Vector3i(0, 0, 0), Vector3i(1, 1, 1));
		Box3i b(Vector3i(1, 0, 0), Vector3i(1, 1, 1));
		ZN_TEST_ASSERT(a.intersects(b) == false);
	}
	{
		Box3i a(Vector3i(0, 0, 0), Vector3i(2, 2, 2));
		Box3i b(Vector3i(1, 0, 0), Vector3i(2, 2, 2));
		ZN_TEST_ASSERT(a.intersects(b));
	}
	{
		Box3i a(Vector3i(-5, 0, 0), Vector3i(10, 1, 1));
		Box3i b(Vector3i(0, -5, 0), Vector3i(1, 10, 1));
		ZN_TEST_ASSERT(a.intersects(b));
	}
	{
		Box3i a(Vector3i(-5, 0, 0), Vector3i(10, 1, 1));
		Box3i b(Vector3i(0, -5, 1), Vector3i(1, 10, 1));
		ZN_TEST_ASSERT(a.intersects(b) == false);
	}
}

void test_box3i_for_inner_outline() {
	const Box3i box(-1, 2, 3, 8, 6, 5);

	std::unordered_map<Vector3i, bool> expected_coords;
	const Box3i inner_box = box.padded(-1);
	box.for_each_cell([&expected_coords, inner_box](Vector3i pos) {
		if (!inner_box.contains(pos)) {
			expected_coords.insert({ pos, false });
		}
	});

	box.for_inner_outline([&expected_coords](Vector3i pos) {
		auto it = expected_coords.find(pos);
		ZN_TEST_ASSERT_MSG(it != expected_coords.end(), "Position must be on the inner outline");
		ZN_TEST_ASSERT_MSG(it->second == false, "Position must be unique");
		it->second = true;
	});

	for (auto it = expected_coords.begin(); it != expected_coords.end(); ++it) {
		const bool v = it->second;
		ZN_TEST_ASSERT_MSG(v, "All expected coordinates must have been found");
	}
}

void test_voxel_data_map_paste_fill() {
	static const int voxel_value = 1;
	static const int default_value = 0;
	static const int channel = VoxelBufferInternal::CHANNEL_TYPE;

	VoxelBufferInternal buffer;
	buffer.create(32, 16, 32);
	buffer.fill(voxel_value, channel);

	VoxelDataMap map;
	map.create(0);

	const Box3i box(Vector3i(10, 10, 10), buffer.get_size());

	map.paste(box.pos, buffer, (1 << channel), false, 0, true);

	// All voxels in the area must be as pasted
	const bool is_match =
			box.all_cells_match([&map](const Vector3i &pos) { return map.get_voxel(pos, channel) == voxel_value; });

	ZN_TEST_ASSERT(is_match);

	// Check neighbor voxels to make sure they were not changed
	const Box3i padded_box = box.padded(1);
	bool outside_is_ok = true;
	padded_box.for_inner_outline([&map, &outside_is_ok](const Vector3i &pos) {
		if (map.get_voxel(pos, channel) != default_value) {
			outside_is_ok = false;
		}
	});

	ZN_TEST_ASSERT(outside_is_ok);
}

void test_voxel_data_map_paste_mask() {
	static const int voxel_value = 1;
	static const int masked_value = 2;
	static const int default_value = 0;
	static const int channel = VoxelBufferInternal::CHANNEL_TYPE;

	VoxelBufferInternal buffer;
	buffer.create(32, 16, 32);
	// Fill the inside of the buffer with a value, and outline it with another value, which we'll use as mask
	buffer.fill(masked_value, channel);
	for (int z = 1; z < buffer.get_size().z - 1; ++z) {
		for (int x = 1; x < buffer.get_size().x - 1; ++x) {
			for (int y = 1; y < buffer.get_size().y - 1; ++y) {
				buffer.set_voxel(voxel_value, x, y, z, channel);
			}
		}
	}

	VoxelDataMap map;
	map.create(0);

	const Box3i box(Vector3i(10, 10, 10), buffer.get_size());

	map.paste(box.pos, buffer, (1 << channel), true, masked_value, true);

	// All voxels in the area must be as pasted. Ignoring the outline.
	const bool is_match = box.padded(-1).all_cells_match(
			[&map](const Vector3i &pos) { return map.get_voxel(pos, channel) == voxel_value; });

	/*for (int y = 0; y < buffer->get_size().y; ++y) {
		String line = String("y={0} | ").format(varray(y));
		for (int x = 0; x < buffer->get_size().x; ++x) {
			const int v = buffer->get_voxel(Vector3i(x, y, box.pos.z + 5), channel);
			if (v == default_value) {
				line += "- ";
			} else if (v == voxel_value) {
				line += "O ";
			} else if (v == masked_value) {
				line += "M ";
			}
		}
		print_line(line);
	}

	for (int y = 0; y < 64; ++y) {
		String line = String("y={0} | ").format(varray(y));
		for (int x = 0; x < 64; ++x) {
			const int v = map.get_voxel(Vector3i(x, y, box.pos.z + 5), channel);
			if (v == default_value) {
				line += "- ";
			} else if (v == voxel_value) {
				line += "O ";
			} else if (v == masked_value) {
				line += "M ";
			}
		}
		print_line(line);
	}*/

	ZN_TEST_ASSERT(is_match);

	// Now check the outline voxels, they should be the same as before
	bool outside_is_ok = true;
	box.for_inner_outline([&map, &outside_is_ok](const Vector3i &pos) {
		if (map.get_voxel(pos, channel) != default_value) {
			outside_is_ok = false;
		}
	});

	ZN_TEST_ASSERT(outside_is_ok);
}

void test_voxel_data_map_copy() {
	static const int voxel_value = 1;
	static const int default_value = 0;
	static const int channel = VoxelBufferInternal::CHANNEL_TYPE;

	VoxelDataMap map;
	map.create(0);

	Box3i box(10, 10, 10, 32, 16, 32);
	VoxelBufferInternal buffer;
	buffer.create(box.size);

	// Fill the inside of the buffer with a value, and leave outline to zero,
	// so our buffer isn't just uniform
	for (int z = 1; z < buffer.get_size().z - 1; ++z) {
		for (int x = 1; x < buffer.get_size().x - 1; ++x) {
			for (int y = 1; y < buffer.get_size().y - 1; ++y) {
				buffer.set_voxel(voxel_value, x, y, z, channel);
			}
		}
	}

	map.paste(box.pos, buffer, (1 << channel), true, default_value, true);

	VoxelBufferInternal buffer2;
	buffer2.create(box.size);

	map.copy(box.pos, buffer2, (1 << channel));

	// for (int y = 0; y < buffer2->get_size().y; ++y) {
	// 	String line = String("y={0} | ").format(varray(y));
	// 	for (int x = 0; x < buffer2->get_size().x; ++x) {
	// 		const int v = buffer2->get_voxel(Vector3i(x, y, 5), channel);
	// 		if (v == default_value) {
	// 			line += "- ";
	// 		} else if (v == voxel_value) {
	// 			line += "O ";
	// 		} else {
	// 			line += "X ";
	// 		}
	// 	}
	// 	print_line(line);
	// }

	ZN_TEST_ASSERT(buffer.equals(buffer2));
}

void test_encode_weights_packed_u16() {
	FixedArray<uint8_t, 4> weights;
	// There is data loss of the 4 smaller bits in this encoding,
	// so to test this we may use values greater than 16.
	// There is a compromise in decoding, where we choose that only values multiple of 16 are bijective.
	weights[0] = 1 << 4;
	weights[1] = 5 << 4;
	weights[2] = 10 << 4;
	weights[3] = 15 << 4;
	const uint16_t encoded_weights = encode_weights_to_packed_u16(weights[0], weights[1], weights[2], weights[3]);
	FixedArray<uint8_t, 4> decoded_weights = decode_weights_from_packed_u16(encoded_weights);
	ZN_TEST_ASSERT(weights == decoded_weights);
}

void test_copy_3d_region_zxy() {
	struct L {
		static void compare(Span<const uint16_t> srcs, Vector3i src_size, Vector3i src_min, Vector3i src_max,
				Span<const uint16_t> dsts, Vector3i dst_size, Vector3i dst_min) {
			Vector3i pos;
			for (pos.z = src_min.z; pos.z < src_max.z; ++pos.z) {
				for (pos.x = src_min.x; pos.x < src_max.x; ++pos.x) {
					for (pos.y = src_min.y; pos.y < src_max.y; ++pos.y) {
						const uint16_t srcv = srcs[Vector3iUtil::get_zxy_index(pos, src_size)];
						const uint16_t dstv = dsts[Vector3iUtil::get_zxy_index(pos - src_min + dst_min, dst_size)];
						ZN_TEST_ASSERT(srcv == dstv);
					}
				}
			}
		}
	};
	// Sub-region
	{
		std::vector<uint16_t> src;
		std::vector<uint16_t> dst;
		const Vector3i src_size(8, 8, 8);
		const Vector3i dst_size(3, 4, 5);
		src.resize(Vector3iUtil::get_volume(src_size), 0);
		dst.resize(Vector3iUtil::get_volume(dst_size), 0);
		for (unsigned int i = 0; i < src.size(); ++i) {
			src[i] = i;
		}

		Span<const uint16_t> srcs = to_span_const(src);
		Span<uint16_t> dsts = to_span(dst);
		const Vector3i dst_min(0, 0, 0);
		const Vector3i src_min(2, 1, 0);
		const Vector3i src_max(5, 4, 3);
		copy_3d_region_zxy(dsts, dst_size, dst_min, srcs, src_size, src_min, src_max);

		/*for (pos.y = src_min.y; pos.y < src_max.y; ++pos.y) {
		String s;
		for (pos.x = src_min.x; pos.x < src_max.x; ++pos.x) {
			const uint16_t v = srcs[pos.get_zxy_index(src_size)];
			if (v < 10) {
				s += String("{0}   ").format(varray(v));
			} else if (v < 100) {
				s += String("{0}  ").format(varray(v));
			} else {
				s += String("{0} ").format(varray(v));
			}
		}
		print_line(s);
	}
	print_line("----");
	const Vector3i dst_max = dst_min + (src_max - src_min);
	pos = Vector3i();
	for (pos.y = dst_min.y; pos.y < dst_max.y; ++pos.y) {
		String s;
		for (pos.x = dst_min.x; pos.x < dst_max.x; ++pos.x) {
			const uint16_t v = dsts[pos.get_zxy_index(dst_size)];
			if (v < 10) {
				s += String("{0}   ").format(varray(v));
			} else if (v < 100) {
				s += String("{0}  ").format(varray(v));
			} else {
				s += String("{0} ").format(varray(v));
			}
		}
		print_line(s);
	}*/

		L::compare(srcs, src_size, src_min, src_max, to_span_const(dsts), dst_size, dst_min);
	}
	// Same size, full region
	{
		std::vector<uint16_t> src;
		std::vector<uint16_t> dst;
		const Vector3i src_size(3, 4, 5);
		const Vector3i dst_size(3, 4, 5);
		src.resize(Vector3iUtil::get_volume(src_size), 0);
		dst.resize(Vector3iUtil::get_volume(dst_size), 0);
		for (unsigned int i = 0; i < src.size(); ++i) {
			src[i] = i;
		}

		Span<const uint16_t> srcs = to_span_const(src);
		Span<uint16_t> dsts = to_span(dst);
		const Vector3i dst_min(0, 0, 0);
		const Vector3i src_min(0, 0, 0);
		const Vector3i src_max = src_size;
		copy_3d_region_zxy(dsts, dst_size, dst_min, srcs, src_size, src_min, src_max);

		L::compare(srcs, src_size, src_min, src_max, to_span_const(dsts), dst_size, dst_min);
	}
}

math::Interval get_sdf_range(const VoxelBufferInternal &block) {
	const VoxelBufferInternal::ChannelId channel = VoxelBufferInternal::CHANNEL_SDF;
	math::Interval range = math::Interval::from_single_value(block.get_voxel_f(Vector3i(), channel));
	Vector3i pos;
	const Vector3i size = block.get_size();

	for (pos.z = 0; pos.z < size.z; ++pos.z) {
		for (pos.x = 0; pos.x < size.x; ++pos.x) {
			for (pos.y = 0; pos.y < size.y; ++pos.y) {
				range.add_point(block.get_voxel_f(pos, channel));
			}
		}
	}

	return range;
}

bool check_graph_results_are_equal(VoxelGeneratorGraph &generator1, VoxelGeneratorGraph &generator2, Vector3i origin) {
	{
		const float sd1 = generator1.generate_single(origin, VoxelBufferInternal::CHANNEL_SDF).f;
		const float sd2 = generator2.generate_single(origin, VoxelBufferInternal::CHANNEL_SDF).f;

		if (!Math::is_equal_approx(sd1, sd2)) {
			ZN_PRINT_ERROR(format("sd1: ", sd1));
			ZN_PRINT_ERROR(format("sd2: ", sd1));
			return false;
		}
	}

	const Vector3i block_size(16, 16, 16);

	VoxelBufferInternal block1;
	block1.create(block_size);

	VoxelBufferInternal block2;
	block2.create(block_size);

	// Note, not every graph configuration can be considered invalid when inequal.
	// SDF clipping does create differences that are supposed to be irrelevant for our use cases.
	// So it is important that we test generators with the same SDF clipping options.
	ZN_ASSERT(generator1.get_sdf_clip_threshold() == generator2.get_sdf_clip_threshold());

	generator1.generate_block(VoxelGenerator::VoxelQueryData{ block1, origin, 0 });
	generator2.generate_block(VoxelGenerator::VoxelQueryData{ block2, origin, 0 });

	if (block1.equals(block2)) {
		return true;
	}

	const math::Interval range1 = get_sdf_range(block1);
	const math::Interval range2 = get_sdf_range(block2);
	ZN_PRINT_ERROR(format("When testing box ", Box3i(origin, block_size)));
	ZN_PRINT_ERROR(format("Block1 range: ", range1));
	ZN_PRINT_ERROR(format("Block2 range: ", range2));
	return false;
}

bool check_graph_results_are_equal(VoxelGeneratorGraph &generator1, VoxelGeneratorGraph &generator2) {
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i()));
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i(-8, -8, -8)));
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i(0, 100, 0)));
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i(0, -100, 0)));
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i(100, 0, 0)));
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i(-100, 0, 0)));
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i(100, 100, 100)));
	ZN_TEST_ASSERT(check_graph_results_are_equal(generator1, generator2, Vector3i(-100, -100, -100)));
	return true;
}

void test_voxel_graph_generator_default_graph_compilation() {
	Ref<VoxelGeneratorGraph> generator_debug;
	Ref<VoxelGeneratorGraph> generator;
	{
		generator_debug.instantiate();
		generator_debug->load_plane_preset();
		VoxelGraphRuntime::CompilationResult result = generator_debug->compile(true);
		ZN_TEST_ASSERT_MSG(result.success,
				String("Failed to compile graph: {0}: {1}").format(varray(result.node_id, result.message)));
	}
	{
		generator.instantiate();
		generator->load_plane_preset();
		VoxelGraphRuntime::CompilationResult result = generator->compile(false);
		ZN_TEST_ASSERT_MSG(result.success,
				String("Failed to compile graph: {0}: {1}").format(varray(result.node_id, result.message)));
	}
	if (generator_debug.is_valid() && generator.is_valid()) {
		ZN_TEST_ASSERT(check_graph_results_are_equal(**generator_debug, **generator));
	}
}

void test_voxel_graph_invalid_connection() {
	Ref<VoxelGraphFunction> graph;
	graph.instantiate();

	VoxelGraphFunction &g = **graph;

	const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
	const uint32_t n_add1 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
	const uint32_t n_add2 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
	const uint32_t n_out = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
	g.add_connection(n_x, 0, n_add1, 0);
	g.add_connection(n_add1, 0, n_add2, 0);
	g.add_connection(n_add2, 0, n_out, 0);

	ZN_TEST_ASSERT(g.can_connect(n_add1, 0, n_add2, 1) == true);
	ZN_TEST_ASSERT_MSG(g.can_connect(n_add1, 0, n_add2, 0) == false, "Adding twice the same connection is not allowed");
	ZN_TEST_ASSERT_MSG(g.can_connect(n_x, 0, n_add2, 0) == false,
			"Adding a connection to a port already connected is not allowed");
	ZN_TEST_ASSERT_MSG(g.can_connect(n_add1, 0, n_add1, 1) == false, "Connecting a node to itself is not allowed");
	ZN_TEST_ASSERT_MSG(g.can_connect(n_add2, 0, n_add1, 1) == false, "Creating a cycle is not allowed");
}

void load_graph_with_sphere_on_plane(VoxelGraphFunction &g, float radius) {
	//      X
	//       \
	//  Z --- Sphere --- Union --- OutSDF
	//       /          /
	//      Y --- Plane
	//

	const uint32_t n_in_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2(0, 0));
	const uint32_t n_in_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2(0, 0));
	const uint32_t n_in_z = g.create_node(VoxelGraphFunction::NODE_INPUT_Z, Vector2(0, 0));
	const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2(0, 0));
	const uint32_t n_plane = g.create_node(VoxelGraphFunction::NODE_SDF_PLANE, Vector2());
	const uint32_t n_sphere = g.create_node(VoxelGraphFunction::NODE_SDF_SPHERE, Vector2());
	const uint32_t n_union = g.create_node(VoxelGraphFunction::NODE_SDF_SMOOTH_UNION, Vector2());

	uint32_t union_smoothness_id;
	ZN_ASSERT(VoxelGraphNodeDB::get_singleton().try_get_param_index_from_name(
			VoxelGraphFunction::NODE_SDF_SMOOTH_UNION, "smoothness", union_smoothness_id));

	g.add_connection(n_in_x, 0, n_sphere, 0);
	g.add_connection(n_in_y, 0, n_sphere, 1);
	g.add_connection(n_in_z, 0, n_sphere, 2);
	g.set_node_param(n_sphere, 0, radius);
	g.add_connection(n_in_y, 0, n_plane, 0);
	g.set_node_default_input(n_plane, 1, 0.f);
	g.add_connection(n_sphere, 0, n_union, 0);
	g.add_connection(n_plane, 0, n_union, 1);
	g.set_node_param(n_union, union_smoothness_id, 0.f);
	g.add_connection(n_union, 0, n_out_sdf, 0);
}

void load_graph_with_expression(VoxelGraphFunction &g) {
	const uint32_t in_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2(0, 0));
	const uint32_t in_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2(0, 0));
	const uint32_t in_z = g.create_node(VoxelGraphFunction::NODE_INPUT_Z, Vector2(0, 0));
	const uint32_t out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2(0, 0));
	const uint32_t n_expression = g.create_node(VoxelGraphFunction::NODE_EXPRESSION, Vector2());

	//             0.5
	//                \
	//     0.1   y --- min
	//        \           \
	//   x --- * --- + --- + --- sdf
	//              /
	//     0.2 --- *
	//            /
	//           z

	g.set_node_param(n_expression, 0, "0.1 * x + 0.2 * z + min(y, 0.5)");
	PackedStringArray var_names;
	var_names.push_back("x");
	var_names.push_back("y");
	var_names.push_back("z");
	g.set_expression_node_inputs(n_expression, var_names);

	g.add_connection(in_x, 0, n_expression, 0);
	g.add_connection(in_y, 0, n_expression, 1);
	g.add_connection(in_z, 0, n_expression, 2);
	g.add_connection(n_expression, 0, out_sdf, 0);
}

void load_graph_with_expression_and_noises(VoxelGraphFunction &g, Ref<ZN_FastNoiseLite> *out_zfnl) {
	//                       SdfPreview
	//                      /
	//     X --- FastNoise2D
	//      \/              \
	//      /\               \
	//     Z --- Noise2D ----- a+b+c --- OutputSDF
	//                        /
	//     Y --- SdfPlane ----

	const uint32_t in_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2(0, 0));
	const uint32_t in_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2(0, 0));
	const uint32_t in_z = g.create_node(VoxelGraphFunction::NODE_INPUT_Z, Vector2(0, 0));
	const uint32_t out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2(0, 0));
	const uint32_t n_fn2d = g.create_node(VoxelGraphFunction::NODE_FAST_NOISE_2D, Vector2());
	const uint32_t n_n2d = g.create_node(VoxelGraphFunction::NODE_NOISE_2D, Vector2());
	const uint32_t n_plane = g.create_node(VoxelGraphFunction::NODE_SDF_PLANE, Vector2());
	const uint32_t n_expr = g.create_node(VoxelGraphFunction::NODE_EXPRESSION, Vector2());
	const uint32_t n_preview = g.create_node(VoxelGraphFunction::NODE_SDF_PREVIEW, Vector2());

	g.set_node_param(n_expr, 0, "a+b+c");
	PackedStringArray var_names;
	var_names.push_back("a");
	var_names.push_back("b");
	var_names.push_back("c");
	g.set_expression_node_inputs(n_expr, var_names);

	Ref<ZN_FastNoiseLite> zfnl;
	zfnl.instantiate();
	g.set_node_param(n_fn2d, 0, zfnl);

	Ref<FastNoiseLite> fnl;
	fnl.instantiate();
	g.set_node_param(n_n2d, 0, fnl);

	g.add_connection(in_x, 0, n_fn2d, 0);
	g.add_connection(in_x, 0, n_n2d, 0);
	g.add_connection(in_z, 0, n_fn2d, 1);
	g.add_connection(in_z, 0, n_n2d, 1);
	g.add_connection(in_y, 0, n_plane, 0);
	g.add_connection(n_fn2d, 0, n_expr, 0);
	g.add_connection(n_fn2d, 0, n_preview, 0);
	g.add_connection(n_n2d, 0, n_expr, 1);
	g.add_connection(n_plane, 0, n_expr, 2);
	g.add_connection(n_expr, 0, out_sdf, 0);

	if (out_zfnl != nullptr) {
		*out_zfnl = zfnl;
	}
}

void load_graph_with_clamp(VoxelGraphFunction &g, float ramp_half_size) {
	// Two planes of different height, with a 45-degrees ramp along the X axis between them.
	// The plane is higher in negative X, and lower in positive X.
	//
	//   X --- Clamp --- + --- Out
	//                  /
	//                 Y

	const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
	const uint32_t n_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2());
	// Not using CLAMP_C for testing simplification
	const uint32_t n_clamp = g.create_node(VoxelGraphFunction::NODE_CLAMP, Vector2());
	const uint32_t n_add = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
	const uint32_t n_out = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());

	g.set_node_default_input(n_clamp, 1, -ramp_half_size);
	g.set_node_default_input(n_clamp, 2, ramp_half_size);

	g.add_connection(n_x, 0, n_clamp, 0);
	g.add_connection(n_clamp, 0, n_add, 0);
	g.add_connection(n_y, 0, n_add, 1);
	g.add_connection(n_add, 0, n_out, 0);
}

void test_voxel_graph_clamp_simplification() {
	// The CLAMP node is replaced with a CLAMP_C node on compilation.
	// This tests that the generator still behaves properly.
	static const float RAMP_HALF_SIZE = 4.f;
	struct L {
		static Ref<VoxelGeneratorGraph> create_graph(bool debug) {
			Ref<VoxelGeneratorGraph> generator;
			generator.instantiate();
			ZN_ASSERT(generator->get_main_function().is_valid());
			load_graph_with_clamp(**generator->get_main_function(), RAMP_HALF_SIZE);
			VoxelGraphRuntime::CompilationResult result = generator->compile(debug);
			ZN_TEST_ASSERT_MSG(result.success,
					String("Failed to compile graph: {0}: {1}").format(varray(result.node_id, result.message)));
			return generator;
		}
		static void test_locations(VoxelGeneratorGraph &g) {
			const VoxelBufferInternal::ChannelId channel = VoxelBufferInternal::CHANNEL_SDF;
			const float sd_on_higher_side_below_ground =
					g.generate_single(Vector3i(-RAMP_HALF_SIZE - 10, 0, 0), channel).f;
			const float sd_on_higher_side_above_ground =
					g.generate_single(Vector3i(-RAMP_HALF_SIZE - 10, RAMP_HALF_SIZE + 2, 0), channel).f;
			const float sd_on_lower_side_above_ground =
					g.generate_single(Vector3i(RAMP_HALF_SIZE + 10, 0, 0), channel).f;
			const float sd_on_lower_side_below_ground =
					g.generate_single(Vector3i(RAMP_HALF_SIZE + 10, -RAMP_HALF_SIZE - 2, 0), channel).f;

			ZN_TEST_ASSERT(sd_on_lower_side_above_ground > 0.f);
			ZN_TEST_ASSERT(sd_on_lower_side_below_ground < 0.f);
			ZN_TEST_ASSERT(sd_on_higher_side_above_ground > 0.f);
			ZN_TEST_ASSERT(sd_on_higher_side_below_ground < 0.f);
		}
	};
	Ref<VoxelGeneratorGraph> generator_debug = L::create_graph(true);
	Ref<VoxelGeneratorGraph> generator = L::create_graph(false);
	ZN_TEST_ASSERT(check_graph_results_are_equal(**generator_debug, **generator));
	L::test_locations(**generator);
	L::test_locations(**generator_debug);
}

void test_voxel_graph_generator_expressions() {
	struct L {
		static Ref<VoxelGeneratorGraph> create_graph(bool debug) {
			Ref<VoxelGeneratorGraph> generator;
			generator.instantiate();
			ZN_ASSERT(generator->get_main_function().is_valid());
			load_graph_with_expression(**generator->get_main_function());
			VoxelGraphRuntime::CompilationResult result = generator->compile(debug);
			ZN_TEST_ASSERT_MSG(result.success,
					String("Failed to compile graph: {0}: {1}").format(varray(result.node_id, result.message)));
			return generator;
		}
	};
	Ref<VoxelGeneratorGraph> generator_debug = L::create_graph(true);
	Ref<VoxelGeneratorGraph> generator = L::create_graph(false);
	ZN_TEST_ASSERT(check_graph_results_are_equal(**generator_debug, **generator));
}

void test_voxel_graph_generator_expressions_2() {
	Ref<ZN_FastNoiseLite> zfnl;
	{
		Ref<VoxelGeneratorGraph> generator_debug;
		{
			generator_debug.instantiate();
			Ref<VoxelGraphFunction> graph = generator_debug->get_main_function();
			ZN_ASSERT(graph.is_valid());
			load_graph_with_expression_and_noises(**graph, &zfnl);
			VoxelGraphRuntime::CompilationResult result = generator_debug->compile(true);
			ZN_TEST_ASSERT_MSG(result.success,
					String("Failed to compile graph: {0}: {1}").format(varray(result.node_id, result.message)));

			generator_debug->generate_single(Vector3i(1, 2, 3), VoxelBufferInternal::CHANNEL_SDF);

			std::vector<VoxelGeneratorGraph::NodeProfilingInfo> profiling_info;
			generator_debug->debug_measure_microseconds_per_voxel(false, &profiling_info);
			ZN_TEST_ASSERT(profiling_info.size() >= 4);
			for (const VoxelGeneratorGraph::NodeProfilingInfo &info : profiling_info) {
				ZN_TEST_ASSERT(graph->has_node(info.node_id));
			}
		}

		Ref<VoxelGeneratorGraph> generator;
		{
			generator.instantiate();
			ZN_ASSERT(generator->get_main_function().is_valid());
			load_graph_with_expression_and_noises(**generator->get_main_function(), nullptr);
			VoxelGraphRuntime::CompilationResult result = generator->compile(false);
			ZN_TEST_ASSERT_MSG(result.success,
					String("Failed to compile graph: {0}: {1}").format(varray(result.node_id, result.message)));
		}

		ZN_TEST_ASSERT(check_graph_results_are_equal(**generator_debug, **generator));
	}

	// Making sure it didn't leak
	ZN_TEST_ASSERT(zfnl.is_valid());
	ZN_TEST_ASSERT(zfnl->get_reference_count() == 1);
}

void test_voxel_graph_generator_texturing() {
	Ref<VoxelGeneratorGraph> generator;
	generator.instantiate();

	VoxelGraphFunction &g = **generator->get_main_function();

	// Plane centered on Y=0, angled 45 degrees, going up towards +X
	// When Y<0, weight0 must be 1 and weight1 must be 0.
	// When Y>0, weight0 must be 0 and weight1 must be 1.
	// When 0<Y<1, weight0 must transition from 1 to 0 and weight1 must transition from 0 to 1.

	/*
	 *        Clamp --- Sub1 --- Weight0
	 *       /      \
	 *  Z   Y        Weight1
	 *       \
	 *  X --- Sub0 --- Sdf
	 *
	 */

	const uint32_t in_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2(0, 0));
	const uint32_t in_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2(0, 0));
	const uint32_t in_z = g.create_node(VoxelGraphFunction::NODE_INPUT_Z, Vector2(0, 0));
	const uint32_t out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2(0, 0));
	const uint32_t n_clamp = g.create_node(VoxelGraphFunction::NODE_CLAMP_C, Vector2(0, 0));
	const uint32_t n_sub0 = g.create_node(VoxelGraphFunction::NODE_SUBTRACT, Vector2(0, 0));
	const uint32_t n_sub1 = g.create_node(VoxelGraphFunction::NODE_SUBTRACT, Vector2(0, 0));
	const uint32_t out_weight0 = g.create_node(VoxelGraphFunction::NODE_OUTPUT_WEIGHT, Vector2(0, 0));
	const uint32_t out_weight1 = g.create_node(VoxelGraphFunction::NODE_OUTPUT_WEIGHT, Vector2(0, 0));

	g.set_node_default_input(n_sub1, 0, 1.0);
	g.set_node_param(n_clamp, 0, 0.0);
	g.set_node_param(n_clamp, 1, 1.0);
	g.set_node_param(out_weight0, 0, 0);
	g.set_node_param(out_weight1, 0, 1);

	g.add_connection(in_y, 0, n_sub0, 0);
	g.add_connection(in_x, 0, n_sub0, 1);
	g.add_connection(n_sub0, 0, out_sdf, 0);
	g.add_connection(in_y, 0, n_clamp, 0);
	g.add_connection(n_clamp, 0, n_sub1, 1);
	g.add_connection(n_sub1, 0, out_weight0, 0);
	g.add_connection(n_clamp, 0, out_weight1, 0);

	VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
	ZN_TEST_ASSERT_MSG(compilation_result.success,
			String("Failed to compile graph: {0}: {1}")
					.format(varray(compilation_result.node_id, compilation_result.message)));

	// Single value tests
	{
		const float sdf_must_be_in_air =
				generator->generate_single(Vector3i(-2, 0, 0), VoxelBufferInternal::CHANNEL_SDF).f;
		const float sdf_must_be_in_ground =
				generator->generate_single(Vector3i(2, 0, 0), VoxelBufferInternal::CHANNEL_SDF).f;
		ZN_TEST_ASSERT(sdf_must_be_in_air > 0.f);
		ZN_TEST_ASSERT(sdf_must_be_in_ground < 0.f);

		uint32_t out_weight0_buffer_index;
		uint32_t out_weight1_buffer_index;
		ZN_TEST_ASSERT(generator->try_get_output_port_address(
				ProgramGraph::PortLocation{ out_weight0, 0 }, out_weight0_buffer_index));
		ZN_TEST_ASSERT(generator->try_get_output_port_address(
				ProgramGraph::PortLocation{ out_weight1, 0 }, out_weight1_buffer_index));

		// Sample two points 1 unit below ground at to heights on the slope

		{
			const float sdf = generator->generate_single(Vector3i(-2, -3, 0), VoxelBufferInternal::CHANNEL_SDF).f;
			ZN_TEST_ASSERT(sdf < 0.f);
			const VoxelGraphRuntime::State &state = VoxelGeneratorGraph::get_last_state_from_current_thread();

			const VoxelGraphRuntime::Buffer &out_weight0_buffer = state.get_buffer(out_weight0_buffer_index);
			const VoxelGraphRuntime::Buffer &out_weight1_buffer = state.get_buffer(out_weight1_buffer_index);

			ZN_TEST_ASSERT(out_weight0_buffer.size >= 1);
			ZN_TEST_ASSERT(out_weight0_buffer.data != nullptr);
			ZN_TEST_ASSERT(out_weight0_buffer.data[0] >= 1.f);

			ZN_TEST_ASSERT(out_weight1_buffer.size >= 1);
			ZN_TEST_ASSERT(out_weight1_buffer.data != nullptr);
			ZN_TEST_ASSERT(out_weight1_buffer.data[0] <= 0.f);
		}
		{
			const float sdf = generator->generate_single(Vector3i(2, 1, 0), VoxelBufferInternal::CHANNEL_SDF).f;
			ZN_TEST_ASSERT(sdf < 0.f);
			const VoxelGraphRuntime::State &state = VoxelGeneratorGraph::get_last_state_from_current_thread();

			const VoxelGraphRuntime::Buffer &out_weight0_buffer = state.get_buffer(out_weight0_buffer_index);
			const VoxelGraphRuntime::Buffer &out_weight1_buffer = state.get_buffer(out_weight1_buffer_index);

			ZN_TEST_ASSERT(out_weight0_buffer.size >= 1);
			ZN_TEST_ASSERT(out_weight0_buffer.data != nullptr);
			ZN_TEST_ASSERT(out_weight0_buffer.data[0] <= 0.f);

			ZN_TEST_ASSERT(out_weight1_buffer.size >= 1);
			ZN_TEST_ASSERT(out_weight1_buffer.data != nullptr);
			ZN_TEST_ASSERT(out_weight1_buffer.data[0] >= 1.f);
		}
	}

	// Block tests
	{
		// packed U16 format decoding has a slightly lower maximum due to a compromise
		const uint8_t WEIGHT_MAX = 240;

		struct L {
			static void check_weights(
					VoxelBufferInternal &buffer, Vector3i pos, bool weight0_must_be_1, bool weight1_must_be_1) {
				const uint16_t encoded_indices = buffer.get_voxel(pos, VoxelBufferInternal::CHANNEL_INDICES);
				const uint16_t encoded_weights = buffer.get_voxel(pos, VoxelBufferInternal::CHANNEL_WEIGHTS);
				const FixedArray<uint8_t, 4> indices = decode_indices_from_packed_u16(encoded_indices);
				const FixedArray<uint8_t, 4> weights = decode_weights_from_packed_u16(encoded_weights);
				for (unsigned int i = 0; i < indices.size(); ++i) {
					switch (indices[i]) {
						case 0:
							if (weight0_must_be_1) {
								ZN_TEST_ASSERT(weights[i] >= WEIGHT_MAX);
							} else {
								ZN_TEST_ASSERT(weights[i] <= 0);
							}
							break;
						case 1:
							if (weight1_must_be_1) {
								ZN_TEST_ASSERT(weights[i] >= WEIGHT_MAX);
							} else {
								ZN_TEST_ASSERT(weights[i] <= 0);
							}
							break;
						default:
							break;
					}
				}
			}

			static void do_block_tests(Ref<VoxelGeneratorGraph> generator) {
				ERR_FAIL_COND(generator.is_null());
				{
					// Block centered on origin
					VoxelBufferInternal buffer;
					buffer.create(Vector3i(16, 16, 16));

					VoxelGenerator::VoxelQueryData query{ buffer, -buffer.get_size() / 2, 0 };
					generator->generate_block(query);

					L::check_weights(buffer, Vector3i(4, 3, 8), true, false);
					L::check_weights(buffer, Vector3i(12, 11, 8), false, true);
				}
				{
					// Two blocks: one above 0, the other below.
					// The point is to check possible bugs due to optimizations.

					// Below 0
					VoxelBufferInternal buffer0;
					{
						buffer0.create(Vector3i(16, 16, 16));
						VoxelGenerator::VoxelQueryData query{ buffer0, Vector3i(0, -16, 0), 0 };
						generator->generate_block(query);
					}

					// Above 0
					VoxelBufferInternal buffer1;
					{
						buffer1.create(Vector3i(16, 16, 16));
						VoxelGenerator::VoxelQueryData query{ buffer1, Vector3i(0, 0, 0), 0 };
						generator->generate_block(query);
					}

					L::check_weights(buffer0, Vector3i(8, 8, 8), true, false);
					L::check_weights(buffer1, Vector3i(8, 8, 8), false, true);
				}
			}
		};

		// Putting state on the stack because the debugger doesnt let me access it
		const VoxelGraphRuntime::State &state = VoxelGeneratorGraph::get_last_state_from_current_thread();

		// Try first without optimization
		generator->set_use_optimized_execution_map(false);
		L::do_block_tests(generator);
		// Try with optimization
		generator->set_use_optimized_execution_map(true);
		L::do_block_tests(generator);
	}
}

void test_voxel_graph_equivalence_merging() {
	{
		// Basic graph with two equivalent branches

		//        1
		//         \
		//    X --- +                         1
		//           \             =>          \
		//        1   + --- Out           X --- + === + --- Out
		//         \ /
		//    X --- +

		Ref<VoxelGeneratorGraph> graph;
		graph.instantiate();
		VoxelGraphFunction &g = **graph->get_main_function();
		const uint32_t n_x1 = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_add1 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
		const uint32_t n_x2 = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_add2 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
		const uint32_t n_add3 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
		const uint32_t n_out = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.set_node_default_input(n_add1, 0, 1.0);
		g.set_node_default_input(n_add2, 0, 1.0);
		g.add_connection(n_x1, 0, n_add1, 1);
		g.add_connection(n_add1, 0, n_add3, 0);
		g.add_connection(n_x2, 0, n_add2, 1);
		g.add_connection(n_add2, 0, n_add3, 1);
		g.add_connection(n_add3, 0, n_out, 0);
		VoxelGraphRuntime::CompilationResult result = graph->compile(false);
		ZN_TEST_ASSERT(result.success);
		ZN_TEST_ASSERT(result.expanded_nodes_count == 4);
		const VoxelSingleValue value = graph->generate_single(Vector3i(10, 0, 0), VoxelBufferInternal::CHANNEL_SDF);
		ZN_TEST_ASSERT(value.f == 22);
	}
	{
		// Same as previous but the X input node is shared

		//          1
		//           \
		//    X ----- +
		//     \       \
		//      \   1   + --- Out
		//       \   \ /
		//        --- +

		Ref<VoxelGeneratorGraph> graph;
		graph.instantiate();
		VoxelGraphFunction &g = **graph->get_main_function();
		const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_add1 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
		const uint32_t n_add2 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
		const uint32_t n_add3 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
		const uint32_t n_out = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.set_node_default_input(n_add1, 0, 1.0);
		g.set_node_default_input(n_add2, 0, 1.0);
		g.add_connection(n_x, 0, n_add1, 1);
		g.add_connection(n_add1, 0, n_add3, 0);
		g.add_connection(n_x, 0, n_add2, 1);
		g.add_connection(n_add2, 0, n_add3, 1);
		g.add_connection(n_add3, 0, n_out, 0);
		VoxelGraphRuntime::CompilationResult result = graph->compile(false);
		ZN_TEST_ASSERT(result.success);
		ZN_TEST_ASSERT(result.expanded_nodes_count == 4);
		const VoxelSingleValue value = graph->generate_single(Vector3i(10, 0, 0), VoxelBufferInternal::CHANNEL_SDF);
		ZN_TEST_ASSERT(value.f == 22);
	}
}

/*void print_sdf_as_ascii(const VoxelBufferInternal &vb) {
	Vector3i pos;
	const VoxelBufferInternal::ChannelId channel = VoxelBufferInternal::CHANNEL_SDF;
	for (pos.y = 0; pos.y < vb.get_size().y; ++pos.y) {
		println(format("Y = {}", pos.y));
		for (pos.z = 0; pos.z < vb.get_size().z; ++pos.z) {
			std::string s;
			std::string s2;
			for (pos.x = 0; pos.x < vb.get_size().x; ++pos.x) {
				const float sd = vb.get_voxel_f(pos, channel);
				char c;
				if (sd < -0.9f) {
					c = '=';
				} else if (sd < 0.0f) {
					c = '-';
				} else if (sd == 0.f) {
					c = ' ';
				} else if (sd < 0.9f) {
					c = '+';
				} else {
					c = '#';
				}
				s += c;
				s += " ";
				std::string n = std::to_string(math::clamp(int(sd * 1000.f), -999, 999));
				while (n.size() < 4) {
					n = " " + n;
				}
				s2 += n;
				s2 += " ";
			}
			s += " | ";
			s += s2;
			println(s);
		}
	}
}*/

/*bool find_different_voxel(const VoxelBufferInternal &vb1, const VoxelBufferInternal &vb2, Vector3i *out_pos,
		unsigned int *out_channel_index) {
	ZN_ASSERT(vb1.get_size() == vb2.get_size());
	Vector3i pos;
	for (pos.y = 0; pos.y < vb1.get_size().y; ++pos.y) {
		for (pos.z = 0; pos.z < vb1.get_size().z; ++pos.z) {
			for (pos.x = 0; pos.x < vb1.get_size().x; ++pos.x) {
				for (unsigned int channel_index = 0; channel_index < VoxelBufferInternal::MAX_CHANNELS;
						++channel_index) {
					const uint64_t v1 = vb1.get_voxel(pos, channel_index);
					const uint64_t v2 = vb2.get_voxel(pos, channel_index);
					if (v1 != v2) {
						if (out_pos != nullptr) {
							*out_pos = pos;
						}
						if (out_channel_index != nullptr) {
							*out_channel_index = channel_index;
						}
						return true;
					}
				}
			}
		}
	}
	return false;
}*/

bool sd_equals_approx(const VoxelBufferInternal &vb1, const VoxelBufferInternal &vb2) {
	const VoxelBufferInternal::ChannelId channel = VoxelBufferInternal::CHANNEL_SDF;
	const VoxelBufferInternal::Depth depth = vb1.get_channel_depth(channel);
	//const float error_margin = 1.1f * VoxelBufferInternal::get_sdf_quantization_scale(depth);
	// There can be a small difference due to scaling operations, so instead of an exact equality, we check approximate
	// equality.
	Vector3i pos;
	for (pos.y = 0; pos.y < vb1.get_size().y; ++pos.y) {
		for (pos.z = 0; pos.z < vb1.get_size().z; ++pos.z) {
			for (pos.x = 0; pos.x < vb1.get_size().x; ++pos.x) {
				switch (depth) {
					case VoxelBufferInternal::DEPTH_8_BIT: {
						const int sd1 = int8_t(vb1.get_voxel(pos, channel));
						const int sd2 = int8_t(vb2.get_voxel(pos, channel));
						if (Math::abs(sd1 - sd2) > 1) {
							return false;
						}
					} break;
					case VoxelBufferInternal::DEPTH_16_BIT: {
						const int sd1 = int16_t(vb1.get_voxel(pos, channel));
						const int sd2 = int16_t(vb2.get_voxel(pos, channel));
						if (Math::abs(sd1 - sd2) > 1) {
							return false;
						}
					} break;
					case VoxelBufferInternal::DEPTH_32_BIT:
					case VoxelBufferInternal::DEPTH_64_BIT: {
						const float sd1 = vb1.get_voxel_f(pos, channel);
						const float sd2 = vb2.get_voxel_f(pos, channel);
						if (!Math::is_equal_approx(sd1, sd2)) {
							return false;
						}
					} break;
				}
			}
		}
	}
	return true;
}

void test_voxel_graph_generate_block_with_input_sdf() {
	static const int BLOCK_SIZE = 16;
	static const float SPHERE_RADIUS = 6;

	struct L {
		static void load_graph(VoxelGraphFunction &g) {
			// Just outputting the input
			const uint32_t n_in_sdf = g.create_node(VoxelGraphFunction::NODE_INPUT_SDF, Vector2());
			const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
			g.add_connection(n_in_sdf, 0, n_out_sdf, 0);
		}

		static void test(bool subdivision_enabled, int subdivision_size) {
			// Create generator
			Ref<VoxelGeneratorGraph> generator;
			generator.instantiate();
			L::load_graph(**generator->get_main_function());
			const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
			ZN_TEST_ASSERT_MSG(compilation_result.success,
					String("Failed to compile graph: {0}: {1}")
							.format(varray(compilation_result.node_id, compilation_result.message)));

			// Create buffer containing part of a sphere
			VoxelBufferInternal buffer;
			buffer.create(Vector3i(BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE));
			const VoxelBufferInternal::ChannelId channel = VoxelBufferInternal::CHANNEL_SDF;
			const VoxelBufferInternal::Depth depth = buffer.get_channel_depth(channel);
			const float sd_scale = VoxelBufferInternal::get_sdf_quantization_scale(depth);
			for (int z = 0; z < buffer.get_size().z; ++z) {
				for (int x = 0; x < buffer.get_size().x; ++x) {
					for (int y = 0; y < buffer.get_size().y; ++y) {
						// Sphere at origin
						const float sd = math::sdf_sphere(Vector3(x, y, z), Vector3(), SPHERE_RADIUS);
						buffer.set_voxel_f(sd * sd_scale, Vector3i(x, y, z), channel);
					}
				}
			}

			// Make a backup before running the generator
			VoxelBufferInternal buffer_before;
			buffer_before.create(buffer.get_size());
			buffer_before.copy_from(buffer);

			generator->set_use_subdivision(subdivision_enabled);
			generator->set_subdivision_size(subdivision_size);
			generator->generate_block(VoxelGenerator::VoxelQueryData{ buffer, Vector3i(), 0 });

			/*if (!buffer.equals(buffer_before)) {
				println("Buffer before:");
				print_sdf_as_ascii(buffer_before);
				println("Buffer after:");
				print_sdf_as_ascii(buffer);
				Vector3i different_pos;
				unsigned int different_channel;
				if (find_different_voxel(buffer_before, buffer, &different_pos, &different_channel)) {
					const uint64_t v1 = buffer_before.get_voxel(different_pos, different_channel);
					const uint64_t v2 = buffer.get_voxel(different_pos, different_channel);
					println(format("Different position: {}, v1={}, v2={}", different_pos, v1, v2));
				}
			}*/
			ZN_TEST_ASSERT(sd_equals_approx(buffer, buffer_before));
		}
	};

	L::test(false, BLOCK_SIZE / 2);
	L::test(true, BLOCK_SIZE / 2);
}

Ref<VoxelGraphFunction> create_pass_through_function() {
	Ref<VoxelGraphFunction> func;
	func.instantiate();
	{
		VoxelGraphFunction &g = **func;
		// Pass through
		// X --- OutSDF
		const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.add_connection(n_x, 0, n_out_sdf, 0);

		func->auto_pick_inputs_and_outputs();
	}
	return func;
}

void test_voxel_graph_functions_pass_through() {
	Ref<VoxelGraphFunction> func = create_pass_through_function();
	Ref<VoxelGeneratorGraph> generator;
	generator.instantiate();
	{
		VoxelGraphFunction &g = **generator->get_main_function();
		// X --- Func --- OutSDF
		const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_f = g.create_function_node(func, Vector2());
		const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.add_connection(n_x, 0, n_f, 0);
		g.add_connection(n_f, 0, n_out_sdf, 0);
	}
	const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
	ZN_TEST_ASSERT_MSG(compilation_result.success,
			String("Failed to compile graph: {0}: {1}")
					.format(varray(compilation_result.node_id, compilation_result.message)));
	const float f = generator->generate_single(Vector3i(42, 0, 0), VoxelBufferInternal::CHANNEL_SDF).f;
	ZN_TEST_ASSERT(f == 42.f);
}

void test_voxel_graph_functions_nested_pass_through() {
	Ref<VoxelGraphFunction> func1 = create_pass_through_function();

	// Minimal function using another
	Ref<VoxelGraphFunction> func2;
	func2.instantiate();
	{
		VoxelGraphFunction &g = **func2;
		// Nested pass through
		// X --- Func1 --- OutSDF
		const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_f = g.create_function_node(func1, Vector2());
		const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.add_connection(n_x, 0, n_f, 0);
		g.add_connection(n_f, 0, n_out_sdf, 0);

		func2->auto_pick_inputs_and_outputs();
	}

	Ref<VoxelGeneratorGraph> generator;
	generator.instantiate();
	{
		VoxelGraphFunction &g = **generator->get_main_function();
		// X --- Func2 --- OutSDF
		const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_f = g.create_function_node(func2, Vector2());
		const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.add_connection(n_x, 0, n_f, 0);
		g.add_connection(n_f, 0, n_out_sdf, 0);
	}
	const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
	ZN_TEST_ASSERT_MSG(compilation_result.success,
			String("Failed to compile graph: {0}: {1}")
					.format(varray(compilation_result.node_id, compilation_result.message)));
	const float f = generator->generate_single(Vector3i(42, 0, 0), VoxelBufferInternal::CHANNEL_SDF).f;
	ZN_TEST_ASSERT(f == 42.f);
}

void test_voxel_graph_functions_autoconnect() {
	Ref<VoxelGraphFunction> func;
	func.instantiate();
	const float sphere_radius = 10.f;
	{
		VoxelGraphFunction &g = **func;
		// Sphere --- OutSDF
		const uint32_t n_sphere = g.create_node(VoxelGraphFunction::NODE_SDF_SPHERE, Vector2());
		const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.add_connection(n_sphere, 0, n_out_sdf, 0);
		g.set_node_param(n_sphere, 0, sphere_radius);

		g.auto_pick_inputs_and_outputs();
	}

	Ref<VoxelGeneratorGraph> generator;
	generator.instantiate();
	const float z_offset = 5.f;
	{
		VoxelGraphFunction &g = **generator->get_main_function();
		//      X (auto)
		//              \
		//  Y (auto) --- Func --- OutSDF
		//              /
		//     Z --- Add+5
		//
		const uint32_t n_z = g.create_node(VoxelGraphFunction::NODE_INPUT_Z, Vector2());
		const uint32_t n_add = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
		const uint32_t n_f = g.create_function_node(func, Vector2());
		const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.add_connection(n_z, 0, n_add, 0);
		g.set_node_default_input(n_add, 1, z_offset);
		g.add_connection(n_add, 0, n_f, 2);
		g.add_connection(n_f, 0, n_out_sdf, 0);
	}
	const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
	ZN_TEST_ASSERT_MSG(compilation_result.success,
			String("Failed to compile graph: {0}: {1}")
					.format(varray(compilation_result.node_id, compilation_result.message)));
	FixedArray<Vector3i, 3> positions;
	positions[0] = Vector3i(1, 1, 1);
	positions[1] = Vector3i(20, 7, -4);
	positions[2] = Vector3i(-5, 0, 18);
	for (const Vector3i &pos : positions) {
		const float sd = generator->generate_single(pos, VoxelBufferInternal::CHANNEL_SDF).f;
		const float expected = math::length(Vector3f(pos.x, pos.y, pos.z + z_offset)) - sphere_radius;
		ZN_TEST_ASSERT(Math::is_equal_approx(sd, expected));
	}
}

void test_voxel_graph_functions_io_mismatch() {
	Ref<VoxelGraphFunction> func;
	func.instantiate();

	// X --- Add --- OutSDF
	//      /
	//     Y
	const uint32_t fn_x = func->create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
	const uint32_t fn_y = func->create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2());
	const uint32_t fn_add = func->create_node(VoxelGraphFunction::NODE_ADD, Vector2());
	const uint32_t fn_out_sdf = func->create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
	func->add_connection(fn_x, 0, fn_add, 0);
	func->add_connection(fn_y, 0, fn_add, 1);
	func->add_connection(fn_add, 0, fn_out_sdf, 0);
	func->auto_pick_inputs_and_outputs();

	Ref<VoxelGeneratorGraph> generator;
	generator.instantiate();
	{
		VoxelGraphFunction &g = **generator->get_main_function();
		// X --- Func --- OutSDF
		//      /
		//     Y
		const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
		const uint32_t n_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2());
		const uint32_t n_f = g.create_function_node(func, Vector2());
		const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
		g.add_connection(n_x, 0, n_f, 0);
		g.add_connection(n_y, 0, n_f, 1);
		g.add_connection(n_f, 0, n_out_sdf, 0);
	}
	{
		const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
		ZN_TEST_ASSERT_MSG(compilation_result.success,
				String("Failed to compile graph: {0}: {1}")
						.format(varray(compilation_result.node_id, compilation_result.message)));
	}

	// Now remove an input from the function, and see how it goes
	{
		FixedArray<VoxelGraphFunction::Port, 1> inputs;
		inputs[0] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_INPUT_X, "x" };
		FixedArray<VoxelGraphFunction::Port, 1> outputs;
		outputs[0] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_OUTPUT_SDF, "sdf" };
		func->set_io_definitions(to_span(inputs), to_span(outputs));
	}
	{
		const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
		// Compiling should fail, but not crash
		ZN_TEST_ASSERT(compilation_result.success == false);
		ZN_PRINT_VERBOSE(format("Compiling failed with message '{}'", compilation_result.message));
	}
	generator->get_main_function()->update_function_nodes(nullptr);
	{
		const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
		// Compiling should work now
		ZN_TEST_ASSERT(compilation_result.success == true);
	}
}

void test_voxel_graph_functions_misc() {
	static const float func_custom_input_defval = 42.f;

	struct L {
		static Ref<VoxelGraphFunction> create_misc_function() {
			Ref<VoxelGraphFunction> func;
			func.instantiate();
			{
				VoxelGraphFunction &g = **func;
				//
				//          X              OutCustom
				//           \
				//       Z -- Add --- Add --- OutSDF
				//                   /
				//           InCustom
				//
				//   Y(unused)
				//
				const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
				const uint32_t n_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2());
				const uint32_t n_z = g.create_node(VoxelGraphFunction::NODE_INPUT_Z, Vector2());
				const uint32_t n_add1 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
				const uint32_t n_add2 = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2());
				const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());
				const uint32_t n_in_custom = g.create_node(VoxelGraphFunction::NODE_CUSTOM_INPUT, Vector2());
				const uint32_t n_out_custom = g.create_node(VoxelGraphFunction::NODE_CUSTOM_OUTPUT, Vector2());

				g.set_node_name(n_in_custom, "custom_input");
				g.set_node_name(n_out_custom, "custom_output");

				g.add_connection(n_x, 0, n_add1, 0);
				g.add_connection(n_z, 0, n_add1, 1);
				g.add_connection(n_add1, 0, n_add2, 0);
				g.add_connection(n_in_custom, 0, n_add2, 1);
				g.add_connection(n_add2, 0, n_out_sdf, 0);
			}
			return func;
		}

		static Ref<VoxelGeneratorGraph> create_generator(Ref<VoxelGraphFunction> func, int input_count) {
			Ref<VoxelGeneratorGraph> generator;
			generator.instantiate();
			//      X
			//       \ 
			//  Z --- Func --- OutSDF
			//
			{
				VoxelGraphFunction &g = **generator->get_main_function();

				const uint32_t n_x = g.create_node(VoxelGraphFunction::NODE_INPUT_X, Vector2());
				const uint32_t n_z = g.create_node(VoxelGraphFunction::NODE_INPUT_Z, Vector2());
				const uint32_t n_f = g.create_function_node(func, Vector2());
				const uint32_t n_out = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2());

				if (input_count == 4) {
					g.set_node_default_input(n_f, 3, func_custom_input_defval);
					// This one shouldn't matter, it's unused, but defined still
					g.set_node_default_input(n_f, 2, 12345);
				}

				g.add_connection(n_x, 0, n_f, 0);
				g.add_connection(n_z, 0, n_f, 1);
				g.add_connection(n_f, 0, n_out, 0);
			}

			return generator;
		}
	};

	// Regular test
	{
		Ref<VoxelGraphFunction> func = L::create_misc_function();
		func->auto_pick_inputs_and_outputs();
		ZN_TEST_ASSERT(func->get_input_definitions().size() == 4);
		ZN_TEST_ASSERT(func->get_output_definitions().size() == 2);

		Ref<VoxelGeneratorGraph> generator = L::create_generator(func, 4);

		const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
		ZN_TEST_ASSERT_MSG(compilation_result.success,
				String("Failed to compile graph: {0}: {1}")
						.format(varray(compilation_result.node_id, compilation_result.message)));

		const Vector3i pos(1, 2, 3);
		const float sd = generator->generate_single(pos, VoxelBufferInternal::CHANNEL_SDF).f;
		const float expected = float(pos.x) + float(pos.z) + func_custom_input_defval;
		ZN_TEST_ASSERT(Math::is_equal_approx(sd, expected));
	}
	// More input nodes than inputs, but should still compile
	{
		Ref<VoxelGraphFunction> func = L::create_misc_function();
		FixedArray<VoxelGraphFunction::Port, 2> inputs;
		inputs[0] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_INPUT_X, "x" };
		inputs[1] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_INPUT, "custom_input" };
		// 2 input nodes don't have corresponding inputs
		FixedArray<VoxelGraphFunction::Port, 2> outputs;
		outputs[0] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_OUTPUT_SDF, "sdf" };
		outputs[1] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_OUTPUT, "custom_output" };
		func->set_io_definitions(to_span(inputs), to_span(outputs));

		Ref<VoxelGeneratorGraph> generator = L::create_generator(func, 2);

		const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
		ZN_TEST_ASSERT_MSG(compilation_result.success,
				String("Failed to compile graph: {0}: {1}")
						.format(varray(compilation_result.node_id, compilation_result.message)));
	}
	// Less I/O nodes than I/Os, but should still compile
	{
		Ref<VoxelGraphFunction> func = L::create_misc_function();
		FixedArray<VoxelGraphFunction::Port, 5> inputs;
		inputs[0] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_INPUT_X, "x" };
		inputs[1] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_INPUT, "custom_input" };
		inputs[2] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_INPUT, "custom_input2" };
		inputs[3] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_INPUT, "custom_input3" };
		inputs[4] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_INPUT, "custom_input4" };
		// 2 input nodes don't have corresponding inputs
		FixedArray<VoxelGraphFunction::Port, 3> outputs;
		outputs[0] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_OUTPUT_SDF, "sdf" };
		outputs[1] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_OUTPUT, "custom_output" };
		outputs[2] = VoxelGraphFunction::Port{ VoxelGraphFunction::NODE_CUSTOM_OUTPUT, "custom_output2" };
		func->set_io_definitions(to_span(inputs), to_span(outputs));

		Ref<VoxelGeneratorGraph> generator = L::create_generator(func, 2);

		const VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
		ZN_TEST_ASSERT_MSG(compilation_result.success,
				String("Failed to compile graph: {0}: {1}")
						.format(varray(compilation_result.node_id, compilation_result.message)));
	}
}

template <typename T>
void get_node_types(const VoxelGraphNodeDB &type_db, std::vector<VoxelGraphFunction::NodeTypeID> &types, T predicate) {
	for (unsigned int i = 0; i < VoxelGraphFunction::NODE_TYPE_COUNT; ++i) {
		const VoxelGraphNodeDB::NodeType &type = type_db.get_type(i);
		if (predicate(type)) {
			types.push_back(VoxelGraphFunction::NodeTypeID(i));
		}
	}
}

// The goal of this test is to find crashes. It will probably cause errors, but should not crash.
void test_voxel_graph_fuzzing() {
	struct L {
		static String make_random_name(RandomPCG &rng) {
			String name;
			const int len = rng.rand() % 8;
			// Note, we let empty names happen.
			for (int i = 0; i < len; ++i) {
				const char c = 'a' + (rng.rand() % ('z' - 'a'));
				name += c;
			}
			return name;
		}

		static void make_random_graph(VoxelGraphFunction &g, RandomPCG &rng, bool allow_custom_io) {
			const int input_count = rng.rand() % 4;
			const int output_count = rng.rand() % 4;
			const int intermediary_node_count = rng.rand() % 8;

			const VoxelGraphNodeDB &type_db = VoxelGraphNodeDB::get_singleton();

			std::vector<VoxelGraphFunction::NodeTypeID> input_types;
			get_node_types(type_db, input_types,
					[](const VoxelGraphNodeDB::NodeType &t) { //
						return t.category == VoxelGraphNodeDB::CATEGORY_INPUT;
					});

			std::vector<VoxelGraphFunction::NodeTypeID> output_types;
			get_node_types(type_db, output_types,
					[](const VoxelGraphNodeDB::NodeType &t) { //
						return t.category == VoxelGraphNodeDB::CATEGORY_OUTPUT;
					});

			if (!allow_custom_io) {
				unordered_remove_value(input_types, VoxelGraphFunction::NODE_CUSTOM_INPUT);
				unordered_remove_value(output_types, VoxelGraphFunction::NODE_CUSTOM_OUTPUT);
			}

			for (int i = 0; i < input_count; ++i) {
				const VoxelGraphFunction::NodeTypeID input_type = input_types[rng.rand() % input_types.size()];
				const uint32_t n = g.create_node(input_type, Vector2());
				g.set_node_name(n, make_random_name(rng));
			}

			for (int i = 0; i < output_count; ++i) {
				const VoxelGraphFunction::NodeTypeID output_type = output_types[rng.rand() % output_types.size()];
				const uint32_t n = g.create_node(output_type, Vector2());
				g.set_node_name(n, make_random_name(rng));
			}

			std::vector<VoxelGraphFunction::NodeTypeID> node_types;
			get_node_types(type_db, node_types,
					[](const VoxelGraphNodeDB::NodeType &t) { //
						return t.category != VoxelGraphNodeDB::CATEGORY_OUTPUT &&
								t.category != VoxelGraphNodeDB::CATEGORY_INPUT;
					});

			for (int i = 0; i < intermediary_node_count; ++i) {
				const VoxelGraphFunction::NodeTypeID type = node_types[rng.rand() % node_types.size()];
				g.create_node(type, Vector2());
			}

			PackedInt32Array node_ids = g.get_node_ids();
			if (node_ids.size() == 0) {
				ZN_PRINT_VERBOSE("Empty graph");
				return;
			}
			const int connection_attempts = rng.rand() % (node_ids.size() + 1);

			for (int i = 0; i < connection_attempts; ++i) {
				const int src_node_id = node_ids[rng.rand() % node_ids.size()];
				const int dst_node_id = node_ids[rng.rand() % node_ids.size()];

				const int src_output_count = g.get_node_output_count(src_node_id);
				const int dst_input_count = g.get_node_input_count(dst_node_id);

				if (src_output_count == 0 || dst_input_count == 0) {
					continue;
				}

				const int src_output_index = rng.rand() % src_output_count;
				const int dst_input_index = rng.rand() % dst_input_count;

				if (g.can_connect(src_node_id, src_output_index, dst_node_id, dst_input_index)) {
					g.add_connection(src_node_id, src_output_index, dst_node_id, dst_input_index);
				}
			}
		}
	};

	const int attempts = 1000;

	RandomPCG rng;
	rng.seed(131183);

	int successful_compiles_count = 0;

	//print_line("--- Begin of zone with possible errors ---");

	for (int i = 0; i < attempts; ++i) {
		ZN_PRINT_VERBOSE(format("Testing random graph #{}", i));
		Ref<VoxelGeneratorGraph> generator;
		generator.instantiate();
		L::make_random_graph(**generator->get_main_function(), rng,
				// Disallowing custom I/Os because VoxelGeneratorGraph cannot handle them at the moment
				false);
		VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(false);
		if (compilation_result.success) {
			generator->generate_single(Vector3i(1, 2, 3), VoxelBufferInternal::CHANNEL_SDF);
		} else {
			++successful_compiles_count;
		}
	}

	//print_line("--- End of zone with possible errors ---");
	print_line(String("Successful random compiles: {0}/{1}").format(varray(successful_compiles_count, attempts)));
}

void test_voxel_graph_sphere_on_plane() {
	static const float RADIUS = 6.f;
	struct L {
		static Ref<VoxelGeneratorGraph> create(bool debug) {
			Ref<VoxelGeneratorGraph> generator;
			generator.instantiate();
			load_graph_with_sphere_on_plane(**generator->get_main_function(), RADIUS);
			VoxelGraphRuntime::CompilationResult compilation_result = generator->compile(debug);
			ZN_TEST_ASSERT_MSG(compilation_result.success,
					String("Failed to compile graph: {0}: {1}")
							.format(varray(compilation_result.node_id, compilation_result.message)));
			return generator;
		}

		static void test_locations(VoxelGeneratorGraph &g) {
			const VoxelBufferInternal::ChannelId channel = VoxelBufferInternal::CHANNEL_SDF;
			const float sd_sky_above_sphere = g.generate_single(Vector3i(0, RADIUS + 5, 0), channel).f;
			const float sd_sky_away_from_sphere = g.generate_single(Vector3i(100, RADIUS + 5, 0), channel).f;
			const float sd_ground_below_sphere = g.generate_single(Vector3i(0, -RADIUS - 5, 0), channel).f;
			const float sd_ground_away_from_sphere = g.generate_single(Vector3i(100, -RADIUS - 5, 0), channel).f;
			const float sd_at_sphere_center = g.generate_single(Vector3i(0, 0, 0), channel).f;
			const float sd_in_sphere_but_higher_than_center =
					g.generate_single(Vector3i(RADIUS / 2, RADIUS / 2, RADIUS / 2), channel).f;

			ZN_TEST_ASSERT(sd_sky_above_sphere > 0.f);
			ZN_TEST_ASSERT(sd_sky_away_from_sphere > 0.f);
			ZN_TEST_ASSERT(sd_ground_below_sphere < 0.f);
			ZN_TEST_ASSERT(sd_ground_away_from_sphere < 0.f);
			ZN_TEST_ASSERT(sd_at_sphere_center < 0.f);
			ZN_TEST_ASSERT(sd_in_sphere_but_higher_than_center < 0.f);
			ZN_TEST_ASSERT(sd_in_sphere_but_higher_than_center > sd_at_sphere_center);
		}
	};
	Ref<VoxelGeneratorGraph> generator_debug = L::create(true);
	Ref<VoxelGeneratorGraph> generator = L::create(false);
	ZN_ASSERT(check_graph_results_are_equal(**generator_debug, **generator));
	L::test_locations(**generator_debug);
	L::test_locations(**generator);
}

#ifdef VOXEL_ENABLE_FAST_NOISE_2

// https://github.com/Zylann/godot_voxel/issues/427
void test_voxel_graph_issue427() {
	Ref<VoxelGeneratorGraph> graph;
	graph.instantiate();
	VoxelGraphFunction &g = **graph->get_main_function();

	const uint32_t n_in_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2()); // 1
	const uint32_t n_sub = g.create_node(VoxelGraphFunction::NODE_SUBTRACT, Vector2()); // 2
	const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2()); // 3
	const uint32_t n_mul = g.create_node(VoxelGraphFunction::NODE_MULTIPLY, Vector2()); // 4
	const uint32_t n_fn2_2d = g.create_node(VoxelGraphFunction::NODE_FAST_NOISE_2_2D, Vector2()); // 5
	const uint32_t n_distance_3d = g.create_node(VoxelGraphFunction::NODE_DISTANCE_3D, Vector2()); // 6

	g.add_connection(n_in_y, 0, n_sub, 0);
	g.add_connection(n_sub, 0, n_out_sdf, 0);
	g.add_connection(n_fn2_2d, 0, n_mul, 0);
	g.add_connection(n_distance_3d, 0, n_mul, 1);
	// Was crashing after adding this connection
	g.add_connection(n_mul, 0, n_sub, 1);

	VoxelGraphRuntime::CompilationResult result = graph->compile(true);
	ZN_TEST_ASSERT(result.success);
}

#ifdef TOOLS_ENABLED

void test_voxel_graph_hash() {
	Ref<VoxelGraphFunction> graph;
	graph.instantiate();
	VoxelGraphFunction &g = **graph;

	const uint32_t n_in_y = g.create_node(VoxelGraphFunction::NODE_INPUT_Y, Vector2()); // 1
	const uint32_t n_add = g.create_node(VoxelGraphFunction::NODE_ADD, Vector2()); // 2
	const uint32_t n_mul = g.create_node(VoxelGraphFunction::NODE_MULTIPLY, Vector2()); // 3
	const uint32_t n_out_sdf = g.create_node(VoxelGraphFunction::NODE_OUTPUT_SDF, Vector2()); // 4
	const uint32_t n_fn2_2d = g.create_node(VoxelGraphFunction::NODE_FAST_NOISE_2_2D, Vector2()); // 5

	// Initial hash
	const uint64_t hash0 = g.get_output_graph_hash();

	// Setting a default input on a node that isn't connected yet to the output
	g.set_node_default_input(n_mul, 1, 2);
	const uint64_t hash1 = g.get_output_graph_hash();
	ZN_TEST_ASSERT(hash1 == hash0);

	// Adding connections up to the output
	g.add_connection(n_in_y, 0, n_add, 0);
	g.add_connection(n_fn2_2d, 0, n_add, 1);
	g.add_connection(n_add, 0, n_mul, 0);
	g.add_connection(n_mul, 0, n_out_sdf, 0);
	const uint64_t hash2 = g.get_output_graph_hash();
	ZN_TEST_ASSERT(hash2 != hash0);

	// Adding only one connection, creating a diamond
	g.add_connection(n_fn2_2d, 0, n_mul, 1);
	const uint64_t hash3 = g.get_output_graph_hash();
	ZN_TEST_ASSERT(hash3 != hash2);

	// Setting a default input
	g.set_node_default_input(n_mul, 1, 4);
	const uint64_t hash4 = g.get_output_graph_hash();
	ZN_TEST_ASSERT(hash4 != hash3);

	// Setting a noise resource property
	Ref<FastNoise2> noise = g.get_node_param(n_fn2_2d, 0);
	noise->set_period(noise->get_period() + 10.f);
	const uint64_t hash5 = g.get_output_graph_hash();
	ZN_TEST_ASSERT(hash5 != hash4);

	// Setting a different noise instance with the same properties
	Ref<FastNoise2> noise2 = noise->duplicate();
	g.set_node_param(n_fn2_2d, 0, noise2);
	const uint64_t hash6 = g.get_output_graph_hash();
	ZN_TEST_ASSERT(hash6 == hash5);
}

#endif // TOOLS_ENABLED
#endif // VOXEL_ENABLE_FAST_NOISE_2

void test_island_finder() {
	const char *cdata = "X X X - X "
						"X X X - - "
						"X X X - - "
						"X X X - - "
						"X X X - - "
						//
						"- - - - - "
						"X X - - - "
						"X X - - - "
						"X X X X X "
						"X X - - X "
						//
						"- - - - - "
						"- - - - - "
						"- - - - - "
						"- - - - - "
						"- - - - - "
						//
						"- - - - - "
						"- - - - - "
						"- - X - - "
						"- - X X - "
						"- - - - - "
						//
						"- - - - - "
						"- - - - - "
						"- - - - - "
						"- - - X - "
						"- - - - - "
			//
			;

	const Vector3i grid_size(5, 5, 5);
	ZN_TEST_ASSERT(Vector3iUtil::get_volume(grid_size) == strlen(cdata) / 2);

	std::vector<int> grid;
	grid.resize(Vector3iUtil::get_volume(grid_size));
	for (unsigned int i = 0; i < grid.size(); ++i) {
		const char c = cdata[i * 2];
		if (c == 'X') {
			grid[i] = 1;
		} else if (c == '-') {
			grid[i] = 0;
		} else {
			ERR_FAIL();
		}
	}

	std::vector<uint8_t> output;
	output.resize(Vector3iUtil::get_volume(grid_size));
	unsigned int label_count;

	IslandFinder island_finder;
	island_finder.scan_3d(
			Box3i(Vector3i(), grid_size),
			[&grid, grid_size](Vector3i pos) {
				const unsigned int i = Vector3iUtil::get_zxy_index(pos, grid_size);
				CRASH_COND(i >= grid.size());
				return grid[i] == 1;
			},
			to_span(output), &label_count);

	// unsigned int i = 0;
	// for (int z = 0; z < grid_size.z; ++z) {
	// 	for (int x = 0; x < grid_size.x; ++x) {
	// 		String s;
	// 		for (int y = 0; y < grid_size.y; ++y) {
	// 			s += String::num_int64(output[i++]);
	// 			s += " ";
	// 		}
	// 		print_line(s);
	// 	}
	// 	print_line("//");
	// }

	ZN_TEST_ASSERT(label_count == 3);
}

void test_unordered_remove_if() {
	struct L {
		static unsigned int count(const std::vector<int> &vec, int v) {
			unsigned int n = 0;
			for (size_t i = 0; i < vec.size(); ++i) {
				if (vec[i] == v) {
					++n;
				}
			}
			return n;
		}
	};
	// Remove one at beginning
	{
		std::vector<int> vec;
		vec.push_back(0);
		vec.push_back(1);
		vec.push_back(2);
		vec.push_back(3);

		unordered_remove_if(vec, [](int v) { return v == 0; });

		ZN_TEST_ASSERT(vec.size() == 3);
		ZN_TEST_ASSERT(
				L::count(vec, 0) == 0 && L::count(vec, 1) == 1 && L::count(vec, 2) == 1 && L::count(vec, 3) == 1);
	}
	// Remove one in middle
	{
		std::vector<int> vec;
		vec.push_back(0);
		vec.push_back(1);
		vec.push_back(2);
		vec.push_back(3);

		unordered_remove_if(vec, [](int v) { return v == 2; });

		ZN_TEST_ASSERT(vec.size() == 3);
		ZN_TEST_ASSERT(
				L::count(vec, 0) == 1 && L::count(vec, 1) == 1 && L::count(vec, 2) == 0 && L::count(vec, 3) == 1);
	}
	// Remove one at end
	{
		std::vector<int> vec;
		vec.push_back(0);
		vec.push_back(1);
		vec.push_back(2);
		vec.push_back(3);

		unordered_remove_if(vec, [](int v) { return v == 3; });

		ZN_TEST_ASSERT(vec.size() == 3);
		ZN_TEST_ASSERT(
				L::count(vec, 0) == 1 && L::count(vec, 1) == 1 && L::count(vec, 2) == 1 && L::count(vec, 3) == 0);
	}
	// Remove multiple
	{
		std::vector<int> vec;
		vec.push_back(0);
		vec.push_back(1);
		vec.push_back(2);
		vec.push_back(3);

		unordered_remove_if(vec, [](int v) { return v == 1 || v == 2; });

		ZN_TEST_ASSERT(vec.size() == 2);
		ZN_TEST_ASSERT(
				L::count(vec, 0) == 1 && L::count(vec, 1) == 0 && L::count(vec, 2) == 0 && L::count(vec, 3) == 1);
	}
	// Remove last
	{
		std::vector<int> vec;
		vec.push_back(0);

		unordered_remove_if(vec, [](int v) { return v == 0; });

		ZN_TEST_ASSERT(vec.size() == 0);
	}
}

void test_instance_data_serialization() {
	struct L {
		static InstanceBlockData::InstanceData create_instance(
				float x, float y, float z, float rotx, float roty, float rotz, float scale) {
			InstanceBlockData::InstanceData d;
			d.transform = Transform3D(
					Basis().rotated(Vector3(rotx, roty, rotz)).scaled(Vector3(scale, scale, scale)), Vector3(x, y, z));
			return d;
		}
	};

	// Create some example data
	InstanceBlockData src_data;
	{
		src_data.position_range = 30;
		{
			InstanceBlockData::LayerData layer;
			layer.id = 1;
			layer.scale_min = 1.f;
			layer.scale_max = 1.f;
			layer.instances.push_back(L::create_instance(0, 0, 0, 0, 0, 0, 1));
			layer.instances.push_back(L::create_instance(10, 0, 0, 3.14, 0, 0, 1));
			layer.instances.push_back(L::create_instance(0, 20, 0, 0, 3.14, 0, 1));
			layer.instances.push_back(L::create_instance(0, 0, 30, 0, 0, 3.14, 1));
			src_data.layers.push_back(layer);
		}
		{
			InstanceBlockData::LayerData layer;
			layer.id = 2;
			layer.scale_min = 1.f;
			layer.scale_max = 4.f;
			layer.instances.push_back(L::create_instance(0, 1, 0, 0, 0, 0, 1));
			layer.instances.push_back(L::create_instance(20, 1, 0, -2.14, 0, 0, 2));
			layer.instances.push_back(L::create_instance(0, 20, 0, 0, -2.14, 0, 3));
			layer.instances.push_back(L::create_instance(0, 1, 20, -1, 0, 2.14, 4));
			src_data.layers.push_back(layer);
		}
	}

	std::vector<uint8_t> serialized_data;

	ZN_TEST_ASSERT(serialize_instance_block_data(src_data, serialized_data));

	InstanceBlockData dst_data;
	ZN_TEST_ASSERT(deserialize_instance_block_data(dst_data, to_span_const(serialized_data)));

	// Compare blocks
	ZN_TEST_ASSERT(src_data.layers.size() == dst_data.layers.size());
	ZN_TEST_ASSERT(dst_data.position_range >= 0.f);
	ZN_TEST_ASSERT(dst_data.position_range == src_data.position_range);

	const float distance_error = math::max(src_data.position_range, InstanceBlockData::POSITION_RANGE_MINIMUM) /
			float(InstanceBlockData::POSITION_RESOLUTION);

	// Compare layers
	for (unsigned int layer_index = 0; layer_index < dst_data.layers.size(); ++layer_index) {
		const InstanceBlockData::LayerData &src_layer = src_data.layers[layer_index];
		const InstanceBlockData::LayerData &dst_layer = dst_data.layers[layer_index];

		ZN_TEST_ASSERT(src_layer.id == dst_layer.id);
		if (src_layer.scale_max - src_layer.scale_min < InstanceBlockData::SIMPLE_11B_V1_SCALE_RANGE_MINIMUM) {
			ZN_TEST_ASSERT(src_layer.scale_min == dst_layer.scale_min);
		} else {
			ZN_TEST_ASSERT(src_layer.scale_min == dst_layer.scale_min);
			ZN_TEST_ASSERT(src_layer.scale_max == dst_layer.scale_max);
		}
		ZN_TEST_ASSERT(src_layer.instances.size() == dst_layer.instances.size());

		const float scale_error = math::max(src_layer.scale_max - src_layer.scale_min,
										  InstanceBlockData::SIMPLE_11B_V1_SCALE_RANGE_MINIMUM) /
				float(InstanceBlockData::SIMPLE_11B_V1_SCALE_RESOLUTION);

		const float rotation_error = 2.f / float(InstanceBlockData::SIMPLE_11B_V1_QUAT_RESOLUTION);

		// Compare instances
		for (unsigned int instance_index = 0; instance_index < src_layer.instances.size(); ++instance_index) {
			const InstanceBlockData::InstanceData &src_instance = src_layer.instances[instance_index];
			const InstanceBlockData::InstanceData &dst_instance = dst_layer.instances[instance_index];

			ZN_TEST_ASSERT(src_instance.transform.origin.distance_to(dst_instance.transform.origin) <= distance_error);

			const Vector3 src_scale = src_instance.transform.basis.get_scale();
			const Vector3 dst_scale = dst_instance.transform.basis.get_scale();
			ZN_TEST_ASSERT(src_scale.distance_to(dst_scale) <= scale_error);

			// Had to normalize here because Godot doesn't want to give you a Quat if the basis is scaled (even
			// uniformly)
			const Quaternion src_rot = src_instance.transform.basis.orthonormalized().get_quaternion();
			const Quaternion dst_rot = dst_instance.transform.basis.orthonormalized().get_quaternion();
			const float rot_dx = Math::abs(src_rot.x - dst_rot.x);
			const float rot_dy = Math::abs(src_rot.y - dst_rot.y);
			const float rot_dz = Math::abs(src_rot.z - dst_rot.z);
			const float rot_dw = Math::abs(src_rot.w - dst_rot.w);
			ZN_TEST_ASSERT(rot_dx <= rotation_error);
			ZN_TEST_ASSERT(rot_dy <= rotation_error);
			ZN_TEST_ASSERT(rot_dz <= rotation_error);
			ZN_TEST_ASSERT(rot_dw <= rotation_error);
		}
	}
}

void test_transform_3d_array_zxy() {
	// YXZ
	int src_grid[] = {
		0, 1, 2, 3, //
		4, 5, 6, 7, //
		8, 9, 10, 11, //

		12, 13, 14, 15, //
		16, 17, 18, 19, //
		20, 21, 22, 23 //
	};
	const Vector3i src_size(3, 4, 2);
	const unsigned int volume = Vector3iUtil::get_volume(src_size);

	FixedArray<int, 24> dst_grid;
	ZN_TEST_ASSERT(dst_grid.size() == volume);

	{
		int expected_dst_grid[] = {
			0, 4, 8, //
			1, 5, 9, //
			2, 6, 10, //
			3, 7, 11, //

			12, 16, 20, //
			13, 17, 21, //
			14, 18, 22, //
			15, 19, 23 //
		};
		const Vector3i expected_dst_size(4, 3, 2);
		IntBasis basis;
		basis.x = Vector3i(0, 1, 0);
		basis.y = Vector3i(1, 0, 0);
		basis.z = Vector3i(0, 0, 1);

		const Vector3i dst_size =
				transform_3d_array_zxy(Span<const int>(src_grid, 0, volume), to_span(dst_grid), src_size, basis);

		ZN_TEST_ASSERT(dst_size == expected_dst_size);

		for (unsigned int i = 0; i < volume; ++i) {
			ZN_TEST_ASSERT(dst_grid[i] == expected_dst_grid[i]);
		}
	}
	{
		int expected_dst_grid[] = {
			3, 2, 1, 0, //
			7, 6, 5, 4, //
			11, 10, 9, 8, //

			15, 14, 13, 12, //
			19, 18, 17, 16, //
			23, 22, 21, 20 //
		};
		const Vector3i expected_dst_size(3, 4, 2);
		IntBasis basis;
		basis.x = Vector3i(1, 0, 0);
		basis.y = Vector3i(0, -1, 0);
		basis.z = Vector3i(0, 0, 1);

		const Vector3i dst_size =
				transform_3d_array_zxy(Span<const int>(src_grid, 0, volume), to_span(dst_grid), src_size, basis);

		ZN_TEST_ASSERT(dst_size == expected_dst_size);

		for (unsigned int i = 0; i < volume; ++i) {
			ZN_TEST_ASSERT(dst_grid[i] == expected_dst_grid[i]);
		}
	}
	{
		int expected_dst_grid[] = {
			15, 14, 13, 12, //
			19, 18, 17, 16, //
			23, 22, 21, 20, //

			3, 2, 1, 0, //
			7, 6, 5, 4, //
			11, 10, 9, 8 //
		};
		const Vector3i expected_dst_size(3, 4, 2);
		IntBasis basis;
		basis.x = Vector3i(1, 0, 0);
		basis.y = Vector3i(0, -1, 0);
		basis.z = Vector3i(0, 0, -1);

		const Vector3i dst_size =
				transform_3d_array_zxy(Span<const int>(src_grid, 0, volume), to_span(dst_grid), src_size, basis);

		ZN_TEST_ASSERT(dst_size == expected_dst_size);

		for (unsigned int i = 0; i < volume; ++i) {
			ZN_TEST_ASSERT(dst_grid[i] == expected_dst_grid[i]);
		}
	}
}

void test_get_curve_monotonic_sections() {
	// This one is a bit annoying to test because Curve has float precision issues stemming from the bake() function
	struct L {
		static bool is_equal_approx(float a, float b) {
			return Math::is_equal_approx(a, b, 2.f * CURVE_RANGE_MARGIN);
		}
	};
	{
		// One segment going up
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0));
		curve->add_point(Vector2(1, 1));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 1);
		ZN_TEST_ASSERT(sections[0].x_min == 0.f);
		ZN_TEST_ASSERT(sections[0].x_max == 1.f);
		ZN_TEST_ASSERT(sections[0].y_min == 0.f);
		ZN_TEST_ASSERT(sections[0].y_max == 1.f);
		{
			math::Interval yi = get_curve_range(**curve, sections, math::Interval(0.f, 1.f));
			ZN_TEST_ASSERT(L::is_equal_approx(yi.min, 0.f));
			ZN_TEST_ASSERT(L::is_equal_approx(yi.max, 1.f));
		}
		{
			math::Interval yi = get_curve_range(**curve, sections, math::Interval(-2.f, 2.f));
			ZN_TEST_ASSERT(L::is_equal_approx(yi.min, 0.f));
			ZN_TEST_ASSERT(L::is_equal_approx(yi.max, 1.f));
		}
		{
			math::Interval xi(0.2f, 0.8f);
			math::Interval yi = get_curve_range(**curve, sections, xi);
			math::Interval yi_expected(curve->sample_baked(xi.min), curve->sample_baked(xi.max));
			ZN_TEST_ASSERT(L::is_equal_approx(yi.min, yi_expected.min));
			ZN_TEST_ASSERT(L::is_equal_approx(yi.max, yi_expected.max));
		}
	}
	{
		// One flat segment
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0));
		curve->add_point(Vector2(1, 0));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 1);
		ZN_TEST_ASSERT(sections[0].x_min == 0.f);
		ZN_TEST_ASSERT(sections[0].x_max == 1.f);
		ZN_TEST_ASSERT(sections[0].y_min == 0.f);
		ZN_TEST_ASSERT(sections[0].y_max == 0.f);
	}
	{
		// Two segments: going up, then flat
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0));
		curve->add_point(Vector2(0.5, 1));
		curve->add_point(Vector2(1, 1));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 1);
	}
	{
		// Two segments: flat, then up
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0));
		curve->add_point(Vector2(0.5, 0));
		curve->add_point(Vector2(1, 1));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 1);
	}
	{
		// Three segments: flat, then up, then flat
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0));
		curve->add_point(Vector2(0.3, 0));
		curve->add_point(Vector2(0.6, 1));
		curve->add_point(Vector2(1, 1));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 1);
	}
	{
		// Three segments: up, down, up
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0));
		curve->add_point(Vector2(0.3, 1));
		curve->add_point(Vector2(0.6, 0));
		curve->add_point(Vector2(1, 1));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 3);
		ZN_TEST_ASSERT(sections[0].x_min == 0.f);
		ZN_TEST_ASSERT(sections[2].x_max == 1.f);
	}
	{
		// Two segments: going up, then down
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0));
		curve->add_point(Vector2(0.5, 1));
		curve->add_point(Vector2(1, 0));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 2);
	}
	{
		// One segment, curved as a parabola going up then down
		Ref<Curve> curve;
		curve.instantiate();
		curve->add_point(Vector2(0, 0), 0.f, 1.f);
		curve->add_point(Vector2(1, 0));
		std::vector<CurveMonotonicSection> sections;
		get_curve_monotonic_sections(**curve, sections);
		ZN_TEST_ASSERT(sections.size() == 2);
		ZN_TEST_ASSERT(sections[0].x_min == 0.f);
		ZN_TEST_ASSERT(sections[0].y_max >= 0.1f);
		ZN_TEST_ASSERT(sections[1].x_max == 1.f);
	}
}

void test_voxel_buffer_create() {
	// This test was a repro for a memory corruption crash. The point of this test is to check it doesn't crash,
	// so there is no particular conditions to check.
	VoxelBufferInternal generated_voxels;
	generated_voxels.create(Vector3i(5, 5, 5));
	generated_voxels.set_voxel_f(-0.7f, 3, 3, 3, VoxelBufferInternal::CHANNEL_SDF);
	generated_voxels.create(Vector3i(16, 16, 18));
	// This was found to cause memory corruption at this point because channels got re-allocated using the new size,
	// but were filled using the old size, which was greater, and accessed out of bounds memory.
	// The old size was used because the `_size` member was assigned too late in the process.
	// The corruption did not cause a crash here, but somewhere random where malloc was used shortly after.
	generated_voxels.create(Vector3i(1, 16, 18));
}

void test_block_serializer() {
	// Create an example buffer
	const Vector3i block_size(8, 9, 10);
	VoxelBufferInternal voxel_buffer;
	voxel_buffer.create(block_size);
	voxel_buffer.fill_area(42, Vector3i(1, 2, 3), Vector3i(5, 5, 5), 0);
	voxel_buffer.fill_area(43, Vector3i(2, 3, 4), Vector3i(6, 6, 6), 0);
	voxel_buffer.fill_area(44, Vector3i(1, 2, 3), Vector3i(5, 5, 5), 1);

	{
		// Serialize without compression wrapper
		BlockSerializer::SerializeResult result = BlockSerializer::serialize(voxel_buffer);
		ZN_TEST_ASSERT(result.success);
		std::vector<uint8_t> data = result.data;

		ZN_TEST_ASSERT(data.size() > 0);
		ZN_TEST_ASSERT(data[0] == BlockSerializer::BLOCK_FORMAT_VERSION);

		// Deserialize
		VoxelBufferInternal deserialized_voxel_buffer;
		ZN_TEST_ASSERT(BlockSerializer::deserialize(to_span_const(data), deserialized_voxel_buffer));

		// Must be equal
		ZN_TEST_ASSERT(voxel_buffer.equals(deserialized_voxel_buffer));
	}
	{
		// Serialize
		BlockSerializer::SerializeResult result = BlockSerializer::serialize_and_compress(voxel_buffer);
		ZN_TEST_ASSERT(result.success);
		std::vector<uint8_t> data = result.data;

		ZN_TEST_ASSERT(data.size() > 0);

		// Deserialize
		VoxelBufferInternal deserialized_voxel_buffer;
		ZN_TEST_ASSERT(BlockSerializer::decompress_and_deserialize(to_span_const(data), deserialized_voxel_buffer));

		// Must be equal
		ZN_TEST_ASSERT(voxel_buffer.equals(deserialized_voxel_buffer));
	}
}

void test_block_serializer_stream_peer() {
	// Create an example buffer
	const Vector3i block_size(8, 9, 10);
	Ref<gd::VoxelBuffer> voxel_buffer;
	voxel_buffer.instantiate();
	voxel_buffer->create(block_size.x, block_size.y, block_size.z);
	voxel_buffer->fill_area(42, Vector3i(1, 2, 3), Vector3i(5, 5, 5), 0);
	voxel_buffer->fill_area(43, Vector3i(2, 3, 4), Vector3i(6, 6, 6), 0);
	voxel_buffer->fill_area(44, Vector3i(1, 2, 3), Vector3i(5, 5, 5), 1);

	Ref<StreamPeerBuffer> peer;
	peer.instantiate();
	//peer->clear();

	Ref<gd::VoxelBlockSerializer> serializer;
	serializer.instantiate();
	const int size = serializer->serialize(peer, voxel_buffer, true);

	PackedByteArray data_array = peer->get_data_array();

	// Client

	Ref<gd::VoxelBuffer> voxel_buffer2;
	voxel_buffer2.instantiate();

	Ref<StreamPeerBuffer> peer2;
	peer2.instantiate();
	peer2->set_data_array(data_array);

	Ref<gd::VoxelBlockSerializer> serializer2;
	serializer2.instantiate();

	serializer2->deserialize(peer2, voxel_buffer2, size, true);

	ZN_TEST_ASSERT(voxel_buffer2->get_buffer().equals(voxel_buffer->get_buffer()));
}

void test_region_file() {
	const int block_size_po2 = 4;
	const int block_size = 1 << block_size_po2;
	const char *region_file_name = "test_region_file.vxr";
	zylann::testing::TestDirectory test_dir;
	ZN_TEST_ASSERT(test_dir.is_valid());
	String region_file_path = test_dir.get_path().path_join(region_file_name);

	struct RandomBlockGenerator {
		RandomPCG rng;

		void generate(VoxelBufferInternal &buffer) {
			buffer.create(Vector3iUtil::create(block_size));
			buffer.set_channel_depth(0, VoxelBufferInternal::DEPTH_16_BIT);

			// Make a block with enough data to take some significant space even if compressed
			for (int z = 0; z < buffer.get_size().z; ++z) {
				for (int x = 0; x < buffer.get_size().x; ++x) {
					for (int y = 0; y < buffer.get_size().y; ++y) {
						buffer.set_voxel(rng.rand() % 256, x, y, z, 0);
					}
				}
			}
		}
	};

	RandomBlockGenerator generator;

	// Create a block of voxels
	VoxelBufferInternal voxel_buffer;
	generator.generate(voxel_buffer);

	{
		RegionFile region_file;

		// Configure region format
		RegionFormat region_format = region_file.get_format();
		region_format.block_size_po2 = block_size_po2;
		for (unsigned int channel_index = 0; channel_index < VoxelBufferInternal::MAX_CHANNELS; ++channel_index) {
			region_format.channel_depths[channel_index] = voxel_buffer.get_channel_depth(channel_index);
		}
		ZN_TEST_ASSERT(region_file.set_format(region_format));

		// Open file
		const Error open_error = region_file.open(region_file_path, true);
		ZN_TEST_ASSERT(open_error == OK);

		// Save block
		const Error save_error = region_file.save_block(Vector3i(1, 2, 3), voxel_buffer);
		ZN_TEST_ASSERT(save_error == OK);

		// Read back
		VoxelBufferInternal loaded_voxel_buffer;
		const Error load_error = region_file.load_block(Vector3i(1, 2, 3), loaded_voxel_buffer);
		ZN_TEST_ASSERT(load_error == OK);

		// Must be equal
		ZN_TEST_ASSERT(voxel_buffer.equals(loaded_voxel_buffer));
	}
	// Load again but using a new region file object
	{
		RegionFile region_file;

		// Open file
		const Error open_error = region_file.open(region_file_path, false);
		ZN_TEST_ASSERT(open_error == OK);

		// Read back
		VoxelBufferInternal loaded_voxel_buffer;
		const Error load_error = region_file.load_block(Vector3i(1, 2, 3), loaded_voxel_buffer);
		ZN_TEST_ASSERT(load_error == OK);

		// Must be equal
		ZN_TEST_ASSERT(voxel_buffer.equals(loaded_voxel_buffer));
	}
	// Save many blocks
	{
		RegionFile region_file;

		// Open file
		const Error open_error = region_file.open(region_file_path, false);
		ZN_TEST_ASSERT(open_error == OK);

		RandomPCG rng;

		std::unordered_map<Vector3i, VoxelBufferInternal> buffers;
		const Vector3i region_size = region_file.get_format().region_size;

		for (int i = 0; i < 1000; ++i) {
			const Vector3i pos = Vector3i( //
					rng.rand() % uint32_t(region_size.x), //
					rng.rand() % uint32_t(region_size.y), //
					rng.rand() % uint32_t(region_size.z) //
			);
			generator.generate(voxel_buffer);

			// Save block
			const Error save_error = region_file.save_block(pos, voxel_buffer);
			ZN_TEST_ASSERT(save_error == OK);

			// Note, the same position can occur twice, we just overwrite
			buffers[pos] = std::move(voxel_buffer);
		}

		// Read back
		for (auto it = buffers.begin(); it != buffers.end(); ++it) {
			VoxelBufferInternal loaded_voxel_buffer;
			const Error load_error = region_file.load_block(it->first, loaded_voxel_buffer);
			ZN_TEST_ASSERT(load_error == OK);
			ZN_TEST_ASSERT(it->second.equals(loaded_voxel_buffer));
		}

		const Error close_error = region_file.close();
		ZN_TEST_ASSERT(close_error == OK);

		// Open file
		const Error open_error2 = region_file.open(region_file_path, false);
		ZN_TEST_ASSERT(open_error2 == OK);

		// Read back again
		for (auto it = buffers.begin(); it != buffers.end(); ++it) {
			VoxelBufferInternal loaded_voxel_buffer;
			const Error load_error = region_file.load_block(it->first, loaded_voxel_buffer);
			ZN_TEST_ASSERT(load_error == OK);
			ZN_TEST_ASSERT(it->second.equals(loaded_voxel_buffer));
		}
	}
}

// Test based on an issue from `I am the Carl` on Discord. It should only not crash or cause errors.
void test_voxel_stream_region_files() {
	const int block_size_po2 = 4;
	const int block_size = 1 << block_size_po2;

	zylann::testing::TestDirectory test_dir;
	ZN_TEST_ASSERT(test_dir.is_valid());

	Ref<VoxelStreamRegionFiles> stream;
	stream.instantiate();
	stream->set_block_size_po2(block_size_po2);
	stream->set_directory(test_dir.get_path());

	RandomPCG rng;

	for (int cycle = 0; cycle < 1000; ++cycle) {
		VoxelBufferInternal buffer;
		buffer.create(block_size, block_size, block_size);

		// Make a block with enough data to take some significant space even if compressed
		for (int z = 0; z < buffer.get_size().z; ++z) {
			for (int x = 0; x < buffer.get_size().x; ++x) {
				for (int y = 0; y < buffer.get_size().y; ++y) {
					buffer.set_voxel(rng.rand() % 256, x, y, z, 0);
				}
			}
		}

		// The position isn't a correct use because it's in voxels, not blocks, but it remains a case that should
		// not cause errors or crash. The same blocks will simply get written to several times.
		VoxelStream::VoxelQueryData q{ buffer, Vector3(cycle, 0, 0), 0, VoxelStream::RESULT_ERROR };
		stream->save_voxel_block(q);
	}
}

#ifdef VOXEL_ENABLE_FAST_NOISE_2

void test_fast_noise_2() {
	// Very basic test. The point is to make sure it doesn't crash, so there is no special condition to check.
	Ref<FastNoise2> noise;
	noise.instantiate();
	float nv = noise->get_noise_2d_single(Vector2(42, 666));
	print_line(String("SIMD level: {0}").format(varray(FastNoise2::get_simd_level_name(noise->get_simd_level()))));
	print_line(String("Noise: {0}").format(varray(nv)));
	Ref<Image> im = Image::create_empty(256, 256, false, Image::FORMAT_RGB8);
	noise->generate_image(im, false);
	//im->save_png("zylann_test_fastnoise2.png");
}

#endif

void test_run_blocky_random_tick() {
	const Box3i voxel_box(Vector3i(-24, -23, -22), Vector3i(64, 40, 40));

	// Create library with tickable voxels
	Ref<VoxelBlockyLibrary> library;
	library.instantiate();
	library->set_voxel_count(3);
	library->create_voxel(0, "air");
	library->create_voxel(1, "non_tickable");
	const int TICKABLE_ID = 2;
	Ref<VoxelBlockyModel> tickable_voxel = library->create_voxel(TICKABLE_ID, "tickable");
	tickable_voxel->set_random_tickable(true);

	// Create test map
	VoxelData data;
	{
		// All blocks of this map will be the same,
		// an interleaving of all block types
		VoxelBufferInternal model_buffer;
		model_buffer.create(Vector3iUtil::create(data.get_block_size()));
		for (int z = 0; z < model_buffer.get_size().z; ++z) {
			for (int x = 0; x < model_buffer.get_size().x; ++x) {
				for (int y = 0; y < model_buffer.get_size().y; ++y) {
					const int block_id = (x + y + z) % 3;
					model_buffer.set_voxel(block_id, x, y, z, VoxelBufferInternal::CHANNEL_TYPE);
				}
			}
		}

		const Box3i world_blocks_box(-4, -4, -4, 8, 8, 8);
		world_blocks_box.for_each_cell_zxy([&data, &model_buffer](Vector3i block_pos) {
			std::shared_ptr<VoxelBufferInternal> buffer = make_shared_instance<VoxelBufferInternal>();
			buffer->create(model_buffer.get_size());
			buffer->copy_from(model_buffer);
			VoxelDataBlock block(buffer, 0);
			block.set_edited(true);
			ZN_TEST_ASSERT(data.try_set_block(block_pos, block));
		});
	}

	struct Callback {
		Box3i voxel_box;
		Box3i pick_box;
		bool first_pick = true;
		bool ok = true;

		Callback(Box3i p_voxel_box) : voxel_box(p_voxel_box) {}

		bool exec(Vector3i pos, int block_id) {
			if (ok) {
				ok = _exec(pos, block_id);
			}
			return ok;
		}

		inline bool _exec(Vector3i pos, int block_id) {
			ZN_TEST_ASSERT_V(block_id == TICKABLE_ID, false);
			ZN_TEST_ASSERT_V(voxel_box.contains(pos), false);
			if (first_pick) {
				first_pick = false;
				pick_box = Box3i(pos, Vector3i(1, 1, 1));
			} else {
				pick_box.merge_with(Box3i(pos, Vector3i(1, 1, 1)));
			}
			return true;
		}
	};

	Callback cb(voxel_box);

	RandomPCG random;
	random.seed(131183);
	VoxelToolTerrain::run_blocky_random_tick_static(
			data, voxel_box, **library, random, 1000, 4, &cb, [](void *self, Vector3i pos, int64_t val) {
				Callback *cb = (Callback *)self;
				return cb->exec(pos, val);
			});

	ZN_TEST_ASSERT(cb.ok);

	// Even though there is randomness, we expect to see at least one hit
	ZN_TEST_ASSERT_MSG(!cb.first_pick, "At least one hit is expected, not none");

	// Check that the points were more or less uniformly sparsed within the provided box.
	// They should, because we populated the world with a checkerboard of tickable voxels.
	// There is randomness at play, so unfortunately we may have to use a margin or pick the right seed,
	// and we only check the enclosing area.
	const int error_margin = 0;
	for (int axis_index = 0; axis_index < Vector3iUtil::AXIS_COUNT; ++axis_index) {
		const int nd = cb.pick_box.pos[axis_index] - voxel_box.pos[axis_index];
		const int pd = cb.pick_box.pos[axis_index] + cb.pick_box.size[axis_index] -
				(voxel_box.pos[axis_index] + voxel_box.size[axis_index]);
		ZN_TEST_ASSERT(Math::abs(nd) <= error_margin);
		ZN_TEST_ASSERT(Math::abs(pd) <= error_margin);
	}
}

void test_flat_map() {
	struct Value {
		int i;
		bool operator==(const Value &other) const {
			return i == other.i;
		}
	};
	typedef FlatMap<int, Value>::Pair Pair;

	std::vector<Pair> sorted_pairs;
	for (int i = 0; i < 100; ++i) {
		sorted_pairs.push_back(Pair{ i, Value{ 1000 * i } });
	}
	const int inexistent_key1 = 101;
	const int inexistent_key2 = -1;

	struct L {
		static bool validate_map(const FlatMap<int, Value> &map, const std::vector<Pair> &sorted_pairs) {
			ZN_TEST_ASSERT_V(sorted_pairs.size() == map.size(), false);
			for (size_t i = 0; i < sorted_pairs.size(); ++i) {
				const Pair expected_pair = sorted_pairs[i];
				ZN_TEST_ASSERT_V(map.has(expected_pair.key), false);
				ZN_TEST_ASSERT_V(map.find(expected_pair.key) != nullptr, false);
				const Value *value = map.find(expected_pair.key);
				ZN_TEST_ASSERT_V(value != nullptr, false);
				ZN_TEST_ASSERT_V(*value == expected_pair.value, false);
			}
			return true;
		}
	};

	std::vector<Pair> shuffled_pairs = sorted_pairs;
	RandomPCG rng;
	rng.seed(131183);
	for (size_t i = 0; i < shuffled_pairs.size(); ++i) {
		size_t dst_i = rng.rand() % shuffled_pairs.size();
		const Pair temp = shuffled_pairs[dst_i];
		shuffled_pairs[dst_i] = shuffled_pairs[i];
		shuffled_pairs[i] = temp;
	}

	{
		// Insert pre-sorted pairs
		FlatMap<int, Value> map;
		for (size_t i = 0; i < sorted_pairs.size(); ++i) {
			const Pair pair = sorted_pairs[i];
			ZN_TEST_ASSERT(map.insert(pair.key, pair.value));
		}
		ZN_TEST_ASSERT(L::validate_map(map, sorted_pairs));
	}
	{
		// Insert random pairs
		FlatMap<int, Value> map;
		for (size_t i = 0; i < shuffled_pairs.size(); ++i) {
			const Pair pair = shuffled_pairs[i];
			ZN_TEST_ASSERT(map.insert(pair.key, pair.value));
		}
		ZN_TEST_ASSERT(L::validate_map(map, sorted_pairs));
	}
	{
		// Insert random pairs with duplicates
		FlatMap<int, Value> map;
		for (size_t i = 0; i < shuffled_pairs.size(); ++i) {
			const Pair pair = shuffled_pairs[i];
			ZN_TEST_ASSERT(map.insert(pair.key, pair.value));
			ZN_TEST_ASSERT_MSG(!map.insert(pair.key, pair.value), "Inserting the key a second time should fail");
		}
		ZN_TEST_ASSERT(L::validate_map(map, sorted_pairs));
	}
	{
		// Init from collection
		FlatMap<int, Value> map;
		map.clear_and_insert(to_span(shuffled_pairs));
		ZN_TEST_ASSERT(L::validate_map(map, sorted_pairs));
	}
	{
		// Inexistent items
		FlatMap<int, Value> map;
		map.clear_and_insert(to_span(shuffled_pairs));
		ZN_TEST_ASSERT(!map.has(inexistent_key1));
		ZN_TEST_ASSERT(!map.has(inexistent_key2));
	}
	{
		// Iteration
		FlatMap<int, Value> map;
		map.clear_and_insert(to_span(shuffled_pairs));
		size_t i = 0;
		for (FlatMap<int, Value>::ConstIterator it = map.begin(); it != map.end(); ++it) {
			ZN_TEST_ASSERT(i < sorted_pairs.size());
			const Pair expected_pair = sorted_pairs[i];
			ZN_TEST_ASSERT(expected_pair.key == it->key);
			ZN_TEST_ASSERT(expected_pair.value == it->value);
			++i;
		}
	}
}

void test_expression_parser() {
	using namespace ExpressionParser;

	{
		Result result = parse("", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("   ", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("42", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root != nullptr);
		ZN_TEST_ASSERT(result.root->type == Node::NUMBER);
		const NumberNode &nn = static_cast<NumberNode &>(*result.root);
		ZN_TEST_ASSERT(Math::is_equal_approx(nn.value, 42.f));
	}
	{
		Result result = parse("()", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("((()))", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("42)", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_UNEXPECTED_TOKEN);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("(42)", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root != nullptr);
		ZN_TEST_ASSERT(result.root->type == Node::NUMBER);
		const NumberNode &nn = static_cast<NumberNode &>(*result.root);
		ZN_TEST_ASSERT(Math::is_equal_approx(nn.value, 42.f));
	}
	{
		Result result = parse("(", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_UNCLOSED_PARENTHESIS);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("(666", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_UNCLOSED_PARENTHESIS);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("1+", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_MISSING_OPERAND_ARGUMENTS);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("++", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_MISSING_OPERAND_ARGUMENTS);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("1 2 3", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_MULTIPLE_OPERANDS);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("???", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_INVALID_TOKEN);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		Result result = parse("1+2-3*4/5", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root != nullptr);
		ZN_TEST_ASSERT(result.root->type == Node::NUMBER);
		const NumberNode &nn = static_cast<NumberNode &>(*result.root);
		ZN_TEST_ASSERT(Math::is_equal_approx(nn.value, 0.6f));
	}
	{
		Result result = parse("1*2-3/4+5", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root != nullptr);
		ZN_TEST_ASSERT(result.root->type == Node::NUMBER);
		const NumberNode &nn = static_cast<NumberNode &>(*result.root);
		ZN_TEST_ASSERT(Math::is_equal_approx(nn.value, 6.25f));
	}
	{
		Result result = parse("(5 - 3)^2 + 2.5/(4 + 6)", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root != nullptr);
		ZN_TEST_ASSERT(result.root->type == Node::NUMBER);
		const NumberNode &nn = static_cast<NumberNode &>(*result.root);
		ZN_TEST_ASSERT(Math::is_equal_approx(nn.value, 4.25f));
	}
	{
		/*
					-
				   / \
				  /   \
				 /     \
				*       -
			   / \     / \
			  4   ^   c   d
				 / \
				+   2
			   / \
			  a   b
		*/
		UniquePtr<VariableNode> node_a = make_unique_instance<VariableNode>("a");
		UniquePtr<VariableNode> node_b = make_unique_instance<VariableNode>("b");
		UniquePtr<OperatorNode> node_add =
				make_unique_instance<OperatorNode>(OperatorNode::ADD, std::move(node_a), std::move(node_b));
		UniquePtr<NumberNode> node_two = make_unique_instance<NumberNode>(2);
		UniquePtr<OperatorNode> node_power =
				make_unique_instance<OperatorNode>(OperatorNode::POWER, std::move(node_add), std::move(node_two));
		UniquePtr<NumberNode> node_four = make_unique_instance<NumberNode>(4);
		UniquePtr<OperatorNode> node_mul =
				make_unique_instance<OperatorNode>(OperatorNode::MULTIPLY, std::move(node_four), std::move(node_power));
		UniquePtr<VariableNode> node_c = make_unique_instance<VariableNode>("c");
		UniquePtr<VariableNode> node_d = make_unique_instance<VariableNode>("d");
		UniquePtr<OperatorNode> node_sub =
				make_unique_instance<OperatorNode>(OperatorNode::SUBTRACT, std::move(node_c), std::move(node_d));
		UniquePtr<OperatorNode> expected_root =
				make_unique_instance<OperatorNode>(OperatorNode::SUBTRACT, std::move(node_mul), std::move(node_sub));

		Result result = parse("4*(a+b)^2-(c-d)", Span<const Function>());
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root != nullptr);
		// {
		// 	const std::string s1 = tree_to_string(*expected_root, Span<const Function>());
		// 	print_line(String(s1.c_str()));
		// 	print_line("---");
		// 	const std::string s2 = tree_to_string(*result.root, Span<const Function>());
		// 	print_line(String(s2.c_str()));
		// }
		ZN_TEST_ASSERT(is_tree_equal(*result.root, *expected_root, Span<const Function>()));
	}
	{
		FixedArray<Function, 2> functions;

		{
			Function f;
			f.name = "sqrt";
			f.id = 0;
			f.argument_count = 1;
			f.func = [](Span<const float> args) { //
				return Math::sqrt(args[0]);
			};
			functions[0] = f;
		}
		{
			Function f;
			f.name = "clamp";
			f.id = 1;
			f.argument_count = 3;
			f.func = [](Span<const float> args) { //
				return math::clamp(args[0], args[1], args[2]);
			};
			functions[1] = f;
		}

		Result result = parse("clamp(sqrt(20 + sqrt(25)), 1, 2.0 * 2.0)", to_span_const(functions));
		ZN_TEST_ASSERT(result.error.id == ERROR_NONE);
		ZN_TEST_ASSERT(result.root != nullptr);
		ZN_TEST_ASSERT(result.root->type == Node::NUMBER);
		const NumberNode &nn = static_cast<NumberNode &>(*result.root);
		ZN_TEST_ASSERT(Math::is_equal_approx(nn.value, 4.f));
	}
	{
		FixedArray<Function, 2> functions;

		const unsigned int F_SIN = 0;
		const unsigned int F_CLAMP = 1;

		{
			Function f;
			f.name = "sin";
			f.id = F_SIN;
			f.argument_count = 1;
			f.func = [](Span<const float> args) { //
				return Math::sin(args[0]);
			};
			functions[0] = f;
		}
		{
			Function f;
			f.name = "clamp";
			f.id = F_CLAMP;
			f.argument_count = 3;
			f.func = [](Span<const float> args) { //
				return math::clamp(args[0], args[1], args[2]);
			};
			functions[1] = f;
		}

		Result result = parse("x+sin(y, clamp(z, 0, 1))", to_span_const(functions));

		ZN_TEST_ASSERT(result.error.id == ERROR_TOO_MANY_ARGUMENTS);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		FixedArray<Function, 1> functions;

		const unsigned int F_CLAMP = 1;

		{
			Function f;
			f.name = "clamp";
			f.id = F_CLAMP;
			f.argument_count = 3;
			f.func = [](Span<const float> args) { //
				return math::clamp(args[0], args[1], args[2]);
			};
			functions[0] = f;
		}

		Result result = parse("clamp(z,", to_span_const(functions));

		ZN_TEST_ASSERT(result.error.id == ERROR_EXPECTED_ARGUMENT);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		FixedArray<Function, 1> functions;

		const unsigned int F_CLAMP = 1;

		{
			Function f;
			f.name = "clamp";
			f.id = F_CLAMP;
			f.argument_count = 3;
			f.func = [](Span<const float> args) { //
				return math::clamp(args[0], args[1], args[2]);
			};
			functions[0] = f;
		}

		Result result = parse("clamp(z)", to_span_const(functions));

		ZN_TEST_ASSERT(result.error.id == ERROR_TOO_FEW_ARGUMENTS);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
	{
		FixedArray<Function, 1> functions;

		const unsigned int F_CLAMP = 1;

		{
			Function f;
			f.name = "clamp";
			f.id = F_CLAMP;
			f.argument_count = 3;
			f.func = [](Span<const float> args) { //
				return math::clamp(args[0], args[1], args[2]);
			};
			functions[0] = f;
		}

		Result result = parse("clamp(z,)", to_span_const(functions));

		ZN_TEST_ASSERT(result.error.id == ERROR_EXPECTED_ARGUMENT);
		ZN_TEST_ASSERT(result.root == nullptr);
	}
}

class CustomMetadataTest : public ICustomVoxelMetadata {
public:
	static const uint8_t ID = VoxelMetadata::TYPE_CUSTOM_BEGIN + 10;

	uint8_t a;
	uint8_t b;
	uint8_t c;

	size_t get_serialized_size() const override {
		// Note, `sizeof(CustomMetadataTest)` gives 16 here. Probably because of vtable
		return 3;
	}

	size_t serialize(Span<uint8_t> dst) const override {
		dst[0] = a;
		dst[1] = b;
		dst[2] = c;
		return get_serialized_size();
	}

	bool deserialize(Span<const uint8_t> src, uint64_t &out_read_size) override {
		a = src[0];
		b = src[1];
		c = src[2];
		out_read_size = get_serialized_size();
		return true;
	}

	virtual ICustomVoxelMetadata *duplicate() {
		CustomMetadataTest *d = ZN_NEW(CustomMetadataTest);
		*d = *this;
		return d;
	}

	bool operator==(const CustomMetadataTest &other) const {
		return a == other.a && b == other.b && c == other.c;
	}
};

void test_voxel_buffer_metadata() {
	// Basic get and set
	{
		VoxelBufferInternal vb;
		vb.create(10, 10, 10);

		VoxelMetadata *meta = vb.get_or_create_voxel_metadata(Vector3i(1, 2, 3));
		ZN_TEST_ASSERT(meta != nullptr);
		meta->set_u64(1234567890);

		const VoxelMetadata *meta2 = vb.get_voxel_metadata(Vector3i(1, 2, 3));
		ZN_TEST_ASSERT(meta2 != nullptr);
		ZN_TEST_ASSERT(meta2->get_type() == meta->get_type());
		ZN_TEST_ASSERT(meta2->get_u64() == meta->get_u64());
	}
	// Serialization
	{
		VoxelBufferInternal vb;
		vb.create(10, 10, 10);

		{
			VoxelMetadata *meta0 = vb.get_or_create_voxel_metadata(Vector3i(1, 2, 3));
			ZN_TEST_ASSERT(meta0 != nullptr);
			meta0->set_u64(1234567890);
		}

		{
			VoxelMetadata *meta1 = vb.get_or_create_voxel_metadata(Vector3i(4, 5, 6));
			ZN_TEST_ASSERT(meta1 != nullptr);
			meta1->clear();
		}

		struct RemoveTypeOnExit {
			~RemoveTypeOnExit() {
				VoxelMetadataFactory::get_singleton().remove_constructor(CustomMetadataTest::ID);
			}
		};
		RemoveTypeOnExit rmtype;
		VoxelMetadataFactory::get_singleton().add_constructor_by_type<CustomMetadataTest>(CustomMetadataTest::ID);
		{
			VoxelMetadata *meta2 = vb.get_or_create_voxel_metadata(Vector3i(7, 8, 9));
			ZN_TEST_ASSERT(meta2 != nullptr);
			CustomMetadataTest *custom = ZN_NEW(CustomMetadataTest);
			custom->a = 10;
			custom->b = 20;
			custom->c = 30;
			meta2->set_custom(CustomMetadataTest::ID, custom);
		}

		BlockSerializer::SerializeResult sresult = BlockSerializer::serialize(vb);
		ZN_TEST_ASSERT(sresult.success);
		std::vector<uint8_t> bytes = sresult.data;

		VoxelBufferInternal rvb;
		ZN_TEST_ASSERT(BlockSerializer::deserialize(to_span(bytes), rvb));

		const FlatMapMoveOnly<Vector3i, VoxelMetadata> &vb_meta_map = vb.get_voxel_metadata();
		const FlatMapMoveOnly<Vector3i, VoxelMetadata> &rvb_meta_map = rvb.get_voxel_metadata();

		ZN_TEST_ASSERT(vb_meta_map.size() == rvb_meta_map.size());

		for (auto it = vb_meta_map.begin(); it != vb_meta_map.end(); ++it) {
			const VoxelMetadata &meta = it->value;
			const VoxelMetadata *rmeta = rvb_meta_map.find(it->key);

			ZN_TEST_ASSERT(rmeta != nullptr);
			ZN_TEST_ASSERT(rmeta->get_type() == meta.get_type());

			switch (meta.get_type()) {
				case VoxelMetadata::TYPE_EMPTY:
					break;
				case VoxelMetadata::TYPE_U64:
					ZN_TEST_ASSERT(meta.get_u64() == rmeta->get_u64());
					break;
				case CustomMetadataTest::ID: {
					const CustomMetadataTest &custom = static_cast<const CustomMetadataTest &>(meta.get_custom());
					const CustomMetadataTest &rcustom = static_cast<const CustomMetadataTest &>(rmeta->get_custom());
					ZN_TEST_ASSERT(custom == rcustom);
				} break;
				default:
					ZN_TEST_ASSERT(false);
					break;
			}
		}
	}
}

void test_voxel_buffer_metadata_gd() {
	// Basic get and set (Godot)
	{
		Ref<gd::VoxelBuffer> vb;
		vb.instantiate();
		vb->create(10, 10, 10);

		Array meta;
		meta.push_back("Hello");
		meta.push_back("World");
		meta.push_back(42);

		vb->set_voxel_metadata(Vector3i(1, 2, 3), meta);

		Array read_meta = vb->get_voxel_metadata(Vector3i(1, 2, 3));
		ZN_TEST_ASSERT(read_meta.size() == meta.size());
		ZN_TEST_ASSERT(read_meta == meta);
	}
	// Serialization (Godot)
	{
		Ref<gd::VoxelBuffer> vb;
		vb.instantiate();
		vb->create(10, 10, 10);

		{
			Array meta0;
			meta0.push_back("Hello");
			meta0.push_back("World");
			meta0.push_back(42);
			vb->set_voxel_metadata(Vector3i(1, 2, 3), meta0);
		}
		{
			Dictionary meta1;
			meta1["One"] = 1;
			meta1["Two"] = 2.5;
			meta1["Three"] = Basis();
			vb->set_voxel_metadata(Vector3i(4, 5, 6), meta1);
		}

		BlockSerializer::SerializeResult sresult = BlockSerializer::serialize(vb->get_buffer());
		ZN_TEST_ASSERT(sresult.success);
		std::vector<uint8_t> bytes = sresult.data;

		Ref<gd::VoxelBuffer> vb2;
		vb2.instantiate();

		ZN_TEST_ASSERT(BlockSerializer::deserialize(to_span(bytes), vb2->get_buffer()));

		ZN_TEST_ASSERT(vb2->get_buffer().equals(vb->get_buffer()));

		// `equals` does not compare metadata at the moment, mainly because it's not trivial and there is no use case
		// for it apart from this test, so do it manually

		const FlatMapMoveOnly<Vector3i, VoxelMetadata> &vb_meta_map = vb->get_buffer().get_voxel_metadata();
		const FlatMapMoveOnly<Vector3i, VoxelMetadata> &vb2_meta_map = vb2->get_buffer().get_voxel_metadata();

		ZN_TEST_ASSERT(vb_meta_map.size() == vb2_meta_map.size());

		for (auto it = vb_meta_map.begin(); it != vb_meta_map.end(); ++it) {
			const VoxelMetadata &meta = it->value;
			ZN_TEST_ASSERT(meta.get_type() == gd::METADATA_TYPE_VARIANT);

			const VoxelMetadata *meta2 = vb2_meta_map.find(it->key);
			ZN_TEST_ASSERT(meta2 != nullptr);
			ZN_TEST_ASSERT(meta2->get_type() == meta.get_type());

			const gd::VoxelMetadataVariant &metav = static_cast<const gd::VoxelMetadataVariant &>(meta.get_custom());
			const gd::VoxelMetadataVariant &meta2v = static_cast<const gd::VoxelMetadataVariant &>(meta2->get_custom());
			ZN_TEST_ASSERT(metav.data == meta2v.data);
		}
	}
}

void test_voxel_mesher_cubes() {
	VoxelBufferInternal vb;
	vb.create(8, 8, 8);
	vb.set_channel_depth(VoxelBufferInternal::CHANNEL_COLOR, VoxelBufferInternal::DEPTH_16_BIT);
	vb.set_voxel(Color8(0, 255, 0, 255).to_u16(), Vector3i(3, 4, 4), VoxelBufferInternal::CHANNEL_COLOR);
	vb.set_voxel(Color8(0, 255, 0, 255).to_u16(), Vector3i(4, 4, 4), VoxelBufferInternal::CHANNEL_COLOR);
	vb.set_voxel(Color8(0, 0, 255, 128).to_u16(), Vector3i(5, 4, 4), VoxelBufferInternal::CHANNEL_COLOR);

	Ref<VoxelMesherCubes> mesher;
	mesher.instantiate();
	mesher->set_color_mode(VoxelMesherCubes::COLOR_RAW);

	VoxelMesher::Input input{ vb, nullptr, nullptr, Vector3i(), 0, false };
	VoxelMesher::Output output;
	mesher->build(output, input);

	const unsigned int opaque_surface_index = VoxelMesherCubes::MATERIAL_OPAQUE;
	const unsigned int transparent_surface_index = VoxelMesherCubes::MATERIAL_TRANSPARENT;

	ZN_TEST_ASSERT(output.surfaces.size() == 2);
	ZN_TEST_ASSERT(output.surfaces[0].arrays.size() > 0);
	ZN_TEST_ASSERT(output.surfaces[1].arrays.size() > 0);

	const PackedVector3Array surface0_vertices = output.surfaces[opaque_surface_index].arrays[Mesh::ARRAY_VERTEX];
	const unsigned int surface0_vertices_count = surface0_vertices.size();

	const PackedVector3Array surface1_vertices = output.surfaces[transparent_surface_index].arrays[Mesh::ARRAY_VERTEX];
	const unsigned int surface1_vertices_count = surface1_vertices.size();

	// println("Surface0:");
	// for (int i = 0; i < surface0_vertices.size(); ++i) {
	// 	println(format("v[{}]: {}", i, surface0_vertices[i]));
	// }
	// println("Surface1:");
	// for (int i = 0; i < surface1_vertices.size(); ++i) {
	// 	println(format("v[{}]: {}", i, surface1_vertices[i]));
	// }

	// Greedy meshing with two cubes of the same color next to each other means it will be a single box.
	// Each side has different normals, so vertices have to be repeated. 6 sides * 4 vertices = 24.
	ZN_TEST_ASSERT(surface0_vertices_count == 24);
	// The transparent cube has less vertices because one of its faces overlaps with a neighbor solid face,
	// so it is culled
	ZN_TEST_ASSERT(surface1_vertices_count == 20);
}

void test_threaded_task_runner() {
	static const uint32_t task_duration_usec = 100'000;

	struct TaskCounter {
		std::atomic_uint32_t max_count;
		std::atomic_uint32_t current_count;
		std::atomic_uint32_t completed_count;

		void reset() {
			max_count = 0;
			current_count = 0;
			completed_count = 0;
		}
	};

	class TestTask : public IThreadedTask {
	public:
		std::shared_ptr<TaskCounter> counter;
		bool completed = false;

		TestTask(std::shared_ptr<TaskCounter> p_counter) : counter(p_counter) {}

		void run(ThreadedTaskContext ctx) override {
			ZN_PROFILE_SCOPE();
			ZN_ASSERT(counter != nullptr);

			++counter->current_count;

			// Update maximum count
			// https://stackoverflow.com/questions/16190078/how-to-atomically-update-a-maximum-value
			unsigned int current_count = counter->current_count;
			unsigned int prev_max = counter->max_count;
			while (prev_max < current_count && !counter->max_count.compare_exchange_weak(prev_max, current_count)) {
				current_count = counter->current_count;
			}

			Thread::sleep_usec(task_duration_usec);

			--counter->current_count;
			++counter->completed_count;
			completed = true;
		}

		void apply_result() override {
			ZN_TEST_ASSERT(completed);
		}
	};

	struct L {
		static void dequeue_tasks(ThreadedTaskRunner &runner) {
			runner.dequeue_completed_tasks([](IThreadedTask *task) {
				ZN_ASSERT(task != nullptr);
				task->apply_result();
				ZN_DELETE(task);
			});
		}
	};

	const unsigned int test_thread_count = 4;
	const unsigned int hw_concurrency = Thread::get_hardware_concurrency();
	if (hw_concurrency < test_thread_count) {
		ZN_PRINT_WARNING(format(
				"Hardware concurrency is {}, smaller than test requirement {}", test_thread_count, hw_concurrency));
	}

	std::shared_ptr<TaskCounter> parallel_counter = make_unique_instance<TaskCounter>();
	std::shared_ptr<TaskCounter> serial_counter = make_unique_instance<TaskCounter>();

	ThreadedTaskRunner runner;
	runner.set_thread_count(test_thread_count);
	runner.set_batch_count(1);
	runner.set_name("Test");

	// Parallel tasks only

	for (unsigned int i = 0; i < 16; ++i) {
		runner.enqueue(ZN_NEW(TestTask(parallel_counter)), false);
	}

	runner.wait_for_all_tasks();
	L::dequeue_tasks(runner);
	ZN_TEST_ASSERT(parallel_counter->completed_count == 16);
	ZN_TEST_ASSERT(parallel_counter->max_count <= test_thread_count);
	ZN_TEST_ASSERT(parallel_counter->current_count == 0);

	// Serial tasks only

	for (unsigned int i = 0; i < 16; ++i) {
		runner.enqueue(ZN_NEW(TestTask(serial_counter)), true);
	}

	runner.wait_for_all_tasks();
	L::dequeue_tasks(runner);
	ZN_TEST_ASSERT(serial_counter->completed_count == 16);
	ZN_TEST_ASSERT(serial_counter->max_count == 1);
	ZN_TEST_ASSERT(serial_counter->current_count == 0);

	// Interleaved

	parallel_counter->reset();
	serial_counter->reset();

	for (unsigned int i = 0; i < 32; ++i) {
		if ((i & 1) == 0) {
			runner.enqueue(ZN_NEW(TestTask(parallel_counter)), false);
		} else {
			runner.enqueue(ZN_NEW(TestTask(serial_counter)), true);
		}
	}

	runner.wait_for_all_tasks();
	L::dequeue_tasks(runner);
	ZN_TEST_ASSERT(parallel_counter->completed_count == 16);
	ZN_TEST_ASSERT(parallel_counter->max_count <= test_thread_count);
	ZN_TEST_ASSERT(parallel_counter->current_count == 0);
	ZN_TEST_ASSERT(serial_counter->completed_count == 16);
	ZN_TEST_ASSERT(serial_counter->max_count == 1);
	ZN_TEST_ASSERT(serial_counter->current_count == 0);
}

void test_task_priority_values() {
	ZN_TEST_ASSERT(TaskPriority(0, 0, 0, 0) < TaskPriority(1, 0, 0, 0));
	ZN_TEST_ASSERT(TaskPriority(0, 0, 0, 0) < TaskPriority(0, 0, 0, 1));
	ZN_TEST_ASSERT(TaskPriority(10, 0, 0, 0) < TaskPriority(0, 10, 0, 0));
	ZN_TEST_ASSERT(TaskPriority(10, 10, 0, 0) < TaskPriority(10, 10, 10, 0));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define VOXEL_TEST(fname)                                                                                              \
	print_line(String("Running {0}").format(varray(#fname)));                                                          \
	fname()

void run_voxel_tests() {
	print_line("------------ Voxel tests begin -------------");

	VOXEL_TEST(test_box3i_intersects);
	VOXEL_TEST(test_box3i_for_inner_outline);
	VOXEL_TEST(test_voxel_data_map_paste_fill);
	VOXEL_TEST(test_voxel_data_map_paste_mask);
	VOXEL_TEST(test_voxel_data_map_copy);
	VOXEL_TEST(test_encode_weights_packed_u16);
	VOXEL_TEST(test_copy_3d_region_zxy);
	VOXEL_TEST(test_voxel_graph_invalid_connection);
	VOXEL_TEST(test_voxel_graph_generator_default_graph_compilation);
	VOXEL_TEST(test_voxel_graph_sphere_on_plane);
	VOXEL_TEST(test_voxel_graph_clamp_simplification);
	VOXEL_TEST(test_voxel_graph_generator_expressions);
	VOXEL_TEST(test_voxel_graph_generator_expressions_2);
	VOXEL_TEST(test_voxel_graph_generator_texturing);
	VOXEL_TEST(test_voxel_graph_equivalence_merging);
	VOXEL_TEST(test_voxel_graph_generate_block_with_input_sdf);
	VOXEL_TEST(test_voxel_graph_functions_pass_through);
	VOXEL_TEST(test_voxel_graph_functions_nested_pass_through);
	VOXEL_TEST(test_voxel_graph_functions_autoconnect);
	VOXEL_TEST(test_voxel_graph_functions_io_mismatch);
	VOXEL_TEST(test_voxel_graph_functions_misc);
	VOXEL_TEST(test_voxel_graph_fuzzing);
#ifdef VOXEL_ENABLE_FAST_NOISE_2
	VOXEL_TEST(test_voxel_graph_issue427);
#ifdef TOOLS_ENABLED
	VOXEL_TEST(test_voxel_graph_hash);
#endif
#endif
	VOXEL_TEST(test_island_finder);
	VOXEL_TEST(test_unordered_remove_if);
	VOXEL_TEST(test_instance_data_serialization);
	VOXEL_TEST(test_transform_3d_array_zxy);
	VOXEL_TEST(test_octree_update);
	VOXEL_TEST(test_octree_find_in_box);
	VOXEL_TEST(test_get_curve_monotonic_sections);
	VOXEL_TEST(test_voxel_buffer_create);
	VOXEL_TEST(test_block_serializer);
	VOXEL_TEST(test_block_serializer_stream_peer);
	VOXEL_TEST(test_region_file);
	VOXEL_TEST(test_voxel_stream_region_files);
#ifdef VOXEL_ENABLE_FAST_NOISE_2
	VOXEL_TEST(test_fast_noise_2);
#endif
	VOXEL_TEST(test_run_blocky_random_tick);
	VOXEL_TEST(test_flat_map);
	VOXEL_TEST(test_expression_parser);
	VOXEL_TEST(test_voxel_buffer_metadata);
	VOXEL_TEST(test_voxel_buffer_metadata_gd);
	VOXEL_TEST(test_voxel_mesher_cubes);
	VOXEL_TEST(test_threaded_task_runner);
	VOXEL_TEST(test_task_priority_values);

	print_line("------------ Voxel tests end -------------");
}

} // namespace zylann::voxel::tests
