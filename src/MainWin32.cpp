#include "MathLib.h"
#include "Vulkan.h"
#include "vulkan/vulkan_core.h"
#include <stdio.h>
#include <stdlib.h>
#pragma warning (disable: 4267)
#pragma warning (disable: 4996)

#include <Windows.h>
#include <cstdio>
#include <map>
#include <set>

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb/stb_rect_pack.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include "Types.h"
#include "Logging.cpp"
#include "Memory.cpp"
#include "String.cpp"
#include "MathLib.cpp"
#include "FileSystem.cpp"
#include "Vulkan.cpp"
#include <vulkan/vulkan_win32.h>

using std::map;
using std::set;

const int WIDTH = 800;
const int HEIGHT = 800;

static inline f32
absf(f32 f) {
	u32 u = *(u32*)(&f);
	u32 absolute = u & 0x7FFFFFFF;
	f32 result = *(f32*)(&absolute);
	return result;
}

// ******
// * UI *
// ******

struct Input {
    bool consolePageDown;
    bool consolePageUp;
    bool consoleNewLine;
    bool consoleToggle;
    bool zoomIn;
    bool zoomOut;
    string cmd;
};

// ******************************************************************************************
// * RESOURCE: Definitions for rendering resources (meshes, fonts, textures, pipelines &c). *
// ******************************************************************************************

struct Uniforms {
    float proj[16];
    float ortho[16];
    float orthoSociogram[16];
    Vec4 eye;
    Vec4 rotation;
};

struct FontInfo {
    const char* name;
    const char* path;
    float size;
};

struct FontInfo fontInfo[] = {
    {
        .name = "default",
        .path = "./fonts/AzeretMono-Medium.ttf",
        .size = 20.f
    },
};

struct Font {
    FontInfo info;
    bool isDirty;
    vector<char> ttfFileContents;

    u32 bitmapSideLength;
    VulkanSampler sampler;

    set<u32> codepointsToLoad;
    set<u32> failedCodepoints;
    map<u32, stbtt_packedchar> dataForCodepoint;
};

struct MeshInfo {
    const char* name;
};

struct Mesh {
    MeshInfo info;

    umm vertexCount;
    umm vertexSizeInFloats;
    vector<f32> vertices;

    umm indexCount;
    vector<u32> indices;
};

enum ResourceType {
    RESOURCE_TYPE_NONE,
    RESOURCE_TYPE_FONT,
    RESOURCE_TYPE_COUNT,
};

struct UniformInfo {
    const char* name;
    ResourceType resourceType;
    const char* resourceName;
};

MeshInfo meshInfo[] = {
    {
        .name = "text",
    },
    {
        .name = "hud_text",
    },
    {
        .name = "lines",
    },
    {
        .name = "console",
    },
    {
        .name = "boxes"
    },
};

PipelineInfo pipelineInfo[] = {
    {
        .name = "text",
        .vertexShaderPath = "shaders/ortho_xy_uv_rgba.vert.spv",
        .fragmentShaderPath = "shaders/text.frag.spv",
        .clockwiseWinding = true,
        .cullBackFaces = false,
        .depthEnabled = false,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    },
    {
        .name = "boxes",
        .vertexShaderPath = "shaders/ortho_xy_uv_rgba.vert.spv",
        .fragmentShaderPath = "shaders/rgba.frag.spv",
        .clockwiseWinding = true,
        .cullBackFaces = false,
        .depthEnabled = false,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    },
    {
        .name = "boxes_sociogram",
        .vertexShaderPath = "shaders/ortho_sociogram_xy_uv_rgba.vert.spv",
        .fragmentShaderPath = "shaders/rgba.frag.spv",
        .clockwiseWinding = true,
        .cullBackFaces = false,
        .depthEnabled = false,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    },
    {
        .name = "lines_sociogram",
        .vertexShaderPath = "shaders/ortho_sociogram_xy_rgba.vert.spv",
        .fragmentShaderPath = "shaders/lines.frag.spv",
        .depthEnabled = false,
        .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
    },
};

struct BrushInfo {
    const char* name;
    const char* meshName;
    const char* pipelineName;
    vector<UniformInfo> uniforms;
    bool disabled;
};

struct Brush {
    BrushInfo info;
};

BrushInfo brushInfo[] = {
    {
        .name = "text",
        .meshName = "text",
        .pipelineName = "text",
        .uniforms = {
            {
                .name = "glyphs",
                .resourceType = RESOURCE_TYPE_FONT,
                .resourceName = "default"
            },
        },
    },
    {
        .name = "hud_text",
        .meshName = "hud_text",
        .pipelineName = "text",
        .uniforms = {
            {
                .name = "glyphs",
                .resourceType = RESOURCE_TYPE_FONT,
                .resourceName = "default"
            },
        },
    },
    {
        .name = "console",
        .meshName = "console",
        .pipelineName = "boxes"
    },
    {
        .name = "boxes",
        .meshName = "boxes",
        .pipelineName = "boxes_sociogram"
    },
    {
        .name = "lines",
        .meshName = "lines",
        .pipelineName = "lines_sociogram",
    },
};

struct Renderer {
    map<const char*, Font> fonts;
    map<const char*, Mesh> meshes;
    map<const char*, VulkanPipeline> pipelines;
    map<const char*, Brush> brushes;
};

#define RENDERER_GET(var, type, name) \
    if (renderer.type.contains(name) == false) { \
        FATAL("%s contains no entry named '%s'", #type, name); \
    } \
    auto& var = renderer.type.at(name)

#define RENDERER_PUT(var, type, name) \
    if (renderer.type.contains(#name) == true) { \
        FATAL("%s already contains an entry named '%s'", #type, name); \
    } \
    renderer.type.insert({ name, var })

// ***********
// * GLOBALS *
// ***********

MemoryArena globalArena;
MemoryArena tempArena;

Input input;

RECT windowRect;
f32 windowWidth;
f32 windowHeight;

u64 frameCount = 0;

#define COLOUR_FROM_HEX(name, r, g, b) Vec4 name = { .x = r/255.f, .y = g/255.f, .z = b/255.f, .w = 1.f }

Vulkan vk;
Vec4 base03 = { .x =      0.f, .y =  43/255.f, .z =  54/255.f, .w = 1.f };
Vec4 base01 = { .x = 88/255.f, .y = 110/255.f, .z = 117/255.f, .w = 1.f };
Vec4 white =  { .x =      1.f, .y =       1.f, .z =       1.f, .w = 1.f };
COLOUR_FROM_HEX(base00,  0x62, 0x62, 0x62);
COLOUR_FROM_HEX(base02,  0x07, 0x36, 0x42);
COLOUR_FROM_HEX(magenta, 0xd3, 0x36, 0x82);
COLOUR_FROM_HEX(green,   0x85, 0x99, 0x00);
COLOUR_FROM_HEX(cyan,    0x2a, 0xa1, 0x98);
COLOUR_FROM_HEX(yellow,  0xb5, 0x89, 0x00);

// ******************************
// * GEOM: Geometry management. *
// ******************************

void
pushLine(Mesh& mesh, Vec2& start, Vec2& end, Vec4& color) {
    umm baseIndex = mesh.vertexCount;

    mesh.vertices.push_back(start.x);
    mesh.vertices.push_back(start.y);
    mesh.vertices.push_back(color.x);
    mesh.vertices.push_back(color.y);
    mesh.vertices.push_back(color.z);
    mesh.vertices.push_back(color.w);
    mesh.vertexCount++;

    mesh.vertices.push_back(end.x);
    mesh.vertices.push_back(end.y);
    mesh.vertices.push_back(color.x);
    mesh.vertices.push_back(color.y);
    mesh.vertices.push_back(color.z);
    mesh.vertices.push_back(color.w);
    mesh.vertexCount++;

    mesh.indices.push_back(baseIndex);
    mesh.indices.push_back(baseIndex+1);
    mesh.indexCount++;
}

void
pushTriangle(Mesh& mesh, Vec2& p0, Vec2& p1, Vec2& p2) {
    umm baseIndex = mesh.vertexCount;

    mesh.vertices.push_back(p0.x);
    mesh.vertices.push_back(p0.y);
    mesh.vertexCount++;

    mesh.vertices.push_back(p1.x);
    mesh.vertices.push_back(p1.y);
    mesh.vertexCount++;

    mesh.vertices.push_back(p2.x);
    mesh.vertices.push_back(p2.y);
    mesh.vertexCount++;

    mesh.indices.push_back(baseIndex);
    mesh.indices.push_back(baseIndex+1);
    mesh.indices.push_back(baseIndex+2);
    mesh.indexCount += 3;
}

void
pushTriangleWithBarycenter(Mesh& mesh, Vec2& p0, Vec2& p1, Vec2& p2) {
    umm baseIndex = mesh.vertexCount;

    mesh.vertices.push_back(p0.x);
    mesh.vertices.push_back(p0.y);
    mesh.vertices.push_back(0);
    mesh.vertices.push_back(0);
    mesh.vertexCount++;

    mesh.vertices.push_back(p1.x);
    mesh.vertices.push_back(p1.y);
    mesh.vertices.push_back(.5f);
    mesh.vertices.push_back(0);
    mesh.vertexCount++;

    mesh.vertices.push_back(p2.x);
    mesh.vertices.push_back(p2.y);
    mesh.vertices.push_back(1);
    mesh.vertices.push_back(1);
    mesh.vertexCount++;

    mesh.indices.push_back(baseIndex);
    mesh.indices.push_back(baseIndex+1);
    mesh.indices.push_back(baseIndex+2);
    mesh.indexCount += 3;
}

void
pushAABox(Mesh& mesh, AABox& box, AABox& tex, Vec4& color) {
    umm baseIndex = mesh.vertexCount;

    // NOTE(jan): Assuming that x grows rightward, and y grows downward.
    //            Number vertices of box clockwise starting at the top
    //            left like v0, v1, v2, and v3.
    // NOTE(jan): This is not implemented as triangle strips because boxes are
    //            mostly disjoint.

    // NOTE(jan): Top-left, v0.
    mesh.vertices.push_back(box.x0);
    mesh.vertices.push_back(box.y0);
    mesh.vertices.push_back(tex.x0);
    mesh.vertices.push_back(tex.y0);
    mesh.vertices.push_back(color.x);
    mesh.vertices.push_back(color.y);
    mesh.vertices.push_back(color.z);
    mesh.vertices.push_back(color.w);
    mesh.vertexCount++;
    // NOTE(jan): Top-right, v1.
    mesh.vertices.push_back(box.x1);
    mesh.vertices.push_back(box.y0);
    mesh.vertices.push_back(tex.x1);
    mesh.vertices.push_back(tex.y0);
    mesh.vertices.push_back(color.x);
    mesh.vertices.push_back(color.y);
    mesh.vertices.push_back(color.z);
    mesh.vertices.push_back(color.w);
    mesh.vertexCount++;
    // NOTE(jan): Bottom-right, v2.
    mesh.vertices.push_back(box.x1);
    mesh.vertices.push_back(box.y1);
    mesh.vertices.push_back(tex.x1);
    mesh.vertices.push_back(tex.y1);
    mesh.vertices.push_back(color.x);
    mesh.vertices.push_back(color.y);
    mesh.vertices.push_back(color.z);
    mesh.vertices.push_back(color.w);
    mesh.vertexCount++;
    // NOTE(jan): Bottom-left, v3.
    mesh.vertices.push_back(box.x0);
    mesh.vertices.push_back(box.y1);
    mesh.vertices.push_back(tex.x0);
    mesh.vertices.push_back(tex.y1);
    mesh.vertices.push_back(color.x);
    mesh.vertices.push_back(color.y);
    mesh.vertices.push_back(color.z);
    mesh.vertices.push_back(color.w);
    mesh.vertexCount++;

    // NOTE(jan): Top-right triangle.
    mesh.indices.push_back(baseIndex + 0);
    mesh.indices.push_back(baseIndex + 1);
    mesh.indices.push_back(baseIndex + 2);
    mesh.indexCount += 3;

    // NOTE(jan): Bottom-left triangle.
    mesh.indices.push_back(baseIndex + 0);
    mesh.indices.push_back(baseIndex + 2);
    mesh.indices.push_back(baseIndex + 3);
    mesh.indexCount += 3;
}

void pushAABox(Mesh& mesh, AABox& box, Vec4& color) {
    AABox tex = {
        .x0 = 0,
        .x1 = 1,
        .y0 = 0,
        .y1 = 1
    };

    pushAABox(mesh, box, tex, color);
}

AABox
pushText(Mesh& mesh, Font& font, AABox& box, String text, Vec4 color) {
    AABox result = {
        .x0 = box.x0,
        .y1 = box.y1
    };

    umm startVertexIndex = mesh.vertices.size();
    umm lineBreaks = 0;

    f32 x = box.x0;
    f32 y = box.y1;

    umm stringIndex = 0;

    while (stringIndex < text.length) {
        char c = text.data[stringIndex];

        // TODO(jan): Better detection of new-lines (unicode).
        if (c == '\n') {
            x = box.x0;
            y += font.info.size;
            stringIndex++;
            continue;
        }
        // TODO(jan): UTF-8 decoding.
        u32 codepoint = (u32)c;

        if (!font.dataForCodepoint.contains(codepoint)) {
            if (!font.failedCodepoints.contains(codepoint)) {
                font.codepointsToLoad.insert(codepoint);
                font.isDirty = true;
            }
            stringIndex++;
            continue;
        }
        stbtt_packedchar cdata = font.dataForCodepoint[codepoint];

        stbtt_aligned_quad quad;
        stbtt_GetPackedQuad(&cdata, font.bitmapSideLength, font.bitmapSideLength, 0, &x, &y, &quad, 0);

        if (quad.x1 > box.x1) {
            lineBreaks++;
            x = box.x0;
            y += font.info.size;
            stbtt_GetPackedQuad(&cdata, font.bitmapSideLength, font.bitmapSideLength, 0, &x, &y, &quad, 0);
        }

        AABox charBox = {
            .x0 = quad.x0,
            .x1 = quad.x1,
            .y0 = quad.y0,
            .y1 = quad.y1
        };
        result.x0 = min(charBox.x0, result.x0);
        result.x1 = fmax(charBox.x1, result.x1);

        AABox tex = {
            .x0 = quad.s0,
            .x1 = quad.s1,
            .y0 = quad.t0,
            .y1 = quad.t1
        };

        pushAABox(mesh, charBox, tex, color);

        stringIndex++;
    }

    if (lineBreaks > 0) {
        for (umm vertexIndex = startVertexIndex; vertexIndex < mesh.vertices.size(); vertexIndex += mesh.vertexSizeInFloats) {
            mesh.vertices[vertexIndex + 1] -= font.info.size * lineBreaks;
        }
    }

    result.y0 = result.y1 - (lineBreaks + 1) * font.info.size;
    return result;
}

// **************************
// * FONT: Font management. *
// **************************

void
packFont(Font& font) {
    INFO("Packing %llu codepoints", font.codepointsToLoad.size());

    font.bitmapSideLength = 512;
    umm bitmapSize = font.bitmapSideLength * font.bitmapSideLength;
    u8* bitmap = new u8[font.bitmapSideLength * font.bitmapSideLength];

    stbtt_pack_context ctxt = {};
    stbtt_PackBegin(&ctxt, bitmap, font.bitmapSideLength, font.bitmapSideLength, 0, 1, NULL);

    for (u32 codepoint: font.codepointsToLoad) {
        if (font.failedCodepoints.contains(codepoint)) continue;

        stbtt_packedchar cdata;
        int result = stbtt_PackFontRange(
            &ctxt,
            (u8*)font.ttfFileContents.data(), 0,
            font.info.size,
            codepoint, 1,
            &cdata
        );
        if (!result) {
            INFO("Could not load codepoint %u", codepoint);
            font.failedCodepoints.insert(codepoint);
        } else {
            font.dataForCodepoint[codepoint] = cdata;
        }
    }

    stbtt_PackEnd(&ctxt);

    if (font.sampler.handle != VK_NULL_HANDLE) {
        destroySampler(vk, font.sampler);
    }

    uploadTexture(vk, font.bitmapSideLength, font.bitmapSideLength, VK_FORMAT_R8_UNORM, bitmap, bitmapSize, font.sampler);
    delete[] bitmap;

    font.isDirty = false;
}

// *******************************
// * SOCIOGRAM: Sociogram stuff. *
// *******************************

struct edge {
    umm from;
    umm to;
};

struct quad_node {
    struct quad_node* tl;
    struct quad_node* tr;
    struct quad_node* bl;
    struct quad_node* br;

    struct Vec2 center;
    u32 count;
};

bool print_stats = false;

vector<Vec2> nodes;
vector<struct edge> edges;

/**
 * box - ordered box
 */
static inline
bool
intersect_box_point(struct AABox box, struct Vec2 point) {
    const bool result = ((point.x >= box.x0) && (point.x <= box.x1) && (point.y >= box.y0) && (point.y <= box.y1));
    // INFO("(%f %f) => (x: %f -> %f, y: %f -> %f): %d", point.x, point.y, box.x0, box.x1, box.y0, box.y1, result);
    return result;
}

// void quad_tree_split(struct quad_node* root, MemoryArena* mem) {
//     const f32 left  = root->bbox.x0;
//     const f32 right = root->bbox.x1;
//     const f32 x_mid = (left + right) / 2.f;

//     const f32 top    = root->bbox.y0;
//     const f32 bottom = root->bbox.y1;
//     const f32 y_mid  = (top + bottom) / 2.f;

//     struct AABox tl_box = { .x0 = left , .x1 = x_mid, .y0 = top  , .y1 = y_mid  };
//     struct AABox tr_box = { .x0 = x_mid, .x1 = right, .y0 = top  , .y1 = y_mid  };
//     struct AABox bl_box = { .x0 = left , .x1 = x_mid, .y0 = y_mid, .y1 = bottom };
//     struct AABox br_box = { .x0 = x_mid, .x1 = right, .y0 = y_mid, .y1 = bottom };

//     root->tl = memoryArenaAllocateStruct(mem, struct quad_node);
//     root->tl->bbox = tl_box;

//     root->tr = memoryArenaAllocateStruct(mem, struct quad_node);
//     root->tr->bbox = tr_box;

//     root->bl = memoryArenaAllocateStruct(mem, struct quad_node);
//     root->bl->bbox = bl_box;

//     root->br = memoryArenaAllocateStruct(mem, struct quad_node);
//     root->br->bbox = br_box;
// }

void quad_tree_split(AABox bbox, AABox& tl, AABox& tr, AABox& bl, AABox& br) {
    const f32 left  = bbox.x0;
    const f32 right = bbox.x1;
    const f32 x_mid = (left + right) / 2.f;

    const f32 top    = bbox.y0;
    const f32 bottom = bbox.y1;
    const f32 y_mid  = (top + bottom) / 2.f;

    tl = { .x0 = left , .x1 = x_mid, .y0 = top  , .y1 = y_mid  };
    tr = { .x0 = x_mid, .x1 = right, .y0 = top  , .y1 = y_mid  };
    bl = { .x0 = left , .x1 = x_mid, .y0 = y_mid, .y1 = bottom };
    br = { .x0 = x_mid, .x1 = right, .y0 = y_mid, .y1 = bottom };
}

bool quad_tree_insert(struct quad_node* root,
                      struct Vec2 node,
                      u32 count,
                      AABox bbox,
                      MemoryArena* mem) {
    bool empty = root->count == 0;
    bool leaf = !(root->tl || root->tr || root->bl || root->br);

    if (leaf) {
        if (empty) {
            root->count = count;
            root->center = node;
            return true;
        } else {
			const f32 epsilon = 1.e-25f;
			const f32 x_delta = absf(node.x - root->center.x);
			const f32 y_delta = absf(node.y - root->center.y);
			const bool x_epsilon = x_delta < epsilon;
			const bool y_epsilon = y_delta < epsilon;

			if (x_epsilon && y_epsilon) {
				root->count += count;
				return true;
			}
        }
    }

    struct AABox tl, tr, bl, br;
    quad_tree_split(bbox, tl, tr, bl, br);

    if (intersect_box_point(tl, node)) {
        if (!root->tl)
            root->tl = memoryArenaAllocateStruct(mem, struct quad_node);
        if (!quad_tree_insert(root->tl, node, count, tl, mem))
            return false;
    } else if (intersect_box_point(tr, node)) {
        if (!root->tr)
            root->tr = memoryArenaAllocateStruct(mem, struct quad_node);
        if (!quad_tree_insert(root->tr, node, count, tr, mem))
            return false;
    } else if (intersect_box_point(bl, node)) {
        if (!root->bl)
            root->bl = memoryArenaAllocateStruct(mem, struct quad_node);
        if (!quad_tree_insert(root->bl, node, count, bl, mem))
            return false;
    } else {
        if (!root->br)
            root->br = memoryArenaAllocateStruct(mem, struct quad_node);
        if (!quad_tree_insert(root->br, node, count, br, mem))
            return false;
    }

    // NOTE(jan): If the node was a leaf, the previous occupant now needs to be moved down the hierarchy.
    if (leaf) {
        return quad_tree_insert(root, root->center, count, bbox, mem);
    } else {
        return true;
    }
}

void
quad_tree_update(struct quad_node* root) {
    bool leaf = !(root->tl || root->tr || root->bl || root->br);
    if (leaf) return;

    if (root->tl) quad_tree_update(root->tl);
    if (root->tr) quad_tree_update(root->tr);
    if (root->bl) quad_tree_update(root->bl);
    if (root->br) quad_tree_update(root->br);

    root->count = (root->tl ? root->tl->count : 0) +
        (root->tr ? root->tr->count : 0) +
        (root->bl ? root->bl->count : 0) +
        (root->br ? root->br->count : 0);
    
    root->center = { 0.f, 0.f };

    if (root->tl) {
        Vec2 tl = root->tl->center;
        vectorScale(root->tl->count, tl);
        vectorAdd(root->center, tl, root->center);
    }

    if (root->tr) {
        Vec2 tr = root->tr->center;
        vectorScale(root->tr->count, tr);
        vectorAdd(root->center, tr, root->center);
    }

    if (root->bl) {
        Vec2 bl = root->bl->center;
        vectorScale(root->bl->count, bl);
        vectorAdd(root->center, bl, root->center);
    }

    if (root->br) {
        Vec2 br = root->br->center;
        vectorScale(root->br->count, br);
        vectorAdd(root->center, br, root->center);
    }

    vectorScale(1.f / root->count, root->center);
}

struct quad_node*
quad_tree_build(AABox bbox, MemoryArena* mem) {
    struct quad_node* root = memoryArenaAllocateStruct(mem, struct quad_node);
    if (nodes.size() < 1) return root;

    for (const Vec2& node: nodes) {
        quad_tree_insert(root, node, 1, bbox, mem);
    }

    quad_tree_update(root);

    return root;
}

struct Vec2
quad_tree_update_node(struct quad_node* root, AABox box, struct Vec2 node) {
    Vec2 result = { 0.f, 0.f };
    if (root == nullptr) return result;

    Vec2 v = { 0.f, 0.f };
    vectorSub(root->center, node, v);
    f32 s = box.x1 - box.x0;
    f32 d = sqrtf(v.x * v.x + v.y * v.y);
    if (d == 0.f) return result;

    float r = s/d;

    bool isLeaf = !(root->tl || root->tr || root->bl || root->br);
    bool isOutside = !intersect_box_point(box, node);
    
    if (isLeaf || (isOutside && (r < .5f))) {
        d = -1.f / (d*d);
        d *= root->count;

        vectorScale(d, v);
        
        result = v;
    } else {
        AABox tl_box, tr_box, bl_box, br_box;
        quad_tree_split(box, tl_box, tr_box, bl_box, br_box);

        Vec2 tl_v = quad_tree_update_node(root->tl, tl_box, node);
        Vec2 tr_v = quad_tree_update_node(root->tr, tr_box, node);
        Vec2 bl_v = quad_tree_update_node(root->bl, bl_box, node);
        Vec2 br_v = quad_tree_update_node(root->br, br_box, node);

        result = { tl_v.x + tr_v.x + bl_v.x + br_v.x, tl_v.y + tr_v.y + bl_v.y + br_v.y };
    }

    return result;
}

void quad_tree_draw(Renderer& renderer, struct quad_node* root, struct AABox bbox) {
    RENDERER_GET(lines, meshes, "lines");

    Vec2 topLeft     = { bbox.x0, bbox.y0 };
    Vec2 topRight    = { bbox.x1, bbox.y0 };
    Vec2 bottomLeft  = { bbox.x0, bbox.y1 };
    Vec2 bottomRight = { bbox.x1, bbox.y1 };
    pushLine(lines, topLeft, topRight, base00);
    pushLine(lines, topRight, bottomRight, base00);
    pushLine(lines, bottomRight, bottomLeft, base00);
    pushLine(lines, bottomLeft, topLeft, base00);

    struct AABox tl, tr, bl, br;
    quad_tree_split(bbox, tl, tr, bl, br);

    if (root->tl) quad_tree_draw(renderer, root->tl, tl);
    if (root->tr) quad_tree_draw(renderer, root->tr, tr);
    if (root->bl) quad_tree_draw(renderer, root->bl, bl);
    if (root->br) quad_tree_draw(renderer, root->br, br);
}

void node_insert_random(umm count) {
    s32 rangeX = windowHeight;
    s32 rangeY = windowHeight;
    s32 startX = rangeX / -2.f;
    s32 startY = rangeY / -2.f;

    for (umm i = 0; i < count; i++) {
        struct Vec2 node = {
            .x = ((f32)rand() / RAND_MAX) * rangeX + startX,
            .y = ((f32)rand() / RAND_MAX) * rangeY + startY
        };
        nodes.push_back(node);
    }

    print_stats = true;

    // for (umm i = 0; i < nodes.size(); i++) {
    //     for (umm j = 0; j < nodes.size(); j++) {
    //         if (i == j) continue;
    //         struct Vec2 I = nodes[i];
    //         struct Vec2 J = nodes[j];
    //         if ((I.x == J.x) && (I.y == J.y)) {
    //             INFO("equal");
    //         }
    //     }
    // }
}

f32 zoomFactor = 1.f;

// ***************************
// * FRAME: Drawing a frame. *
// ***************************

void doFrame(Vulkan& vk, Renderer& renderer) {
    f32 frameStart = getElapsed();

    MemoryArena frameArena = {};

    // NOTE(jan): Acquire swap image.
    uint32_t swapImageIndex = 0;
    auto result = vkAcquireNextImageKHR(
        vk.device,
        vk.swap.handle,
        std::numeric_limits<uint64_t>::max(),
        vk.swap.imageReady,
        VK_NULL_HANDLE,
        &swapImageIndex
    );
    if ((result == VK_SUBOPTIMAL_KHR) ||
        (result == VK_ERROR_OUT_OF_DATE_KHR)) {
        // TODO(jan): Implement resize.
        FATAL("could not acquire next image")
    } else if (result != VK_SUCCESS) {
        FATAL("could not acquire next image")
    }

    if (input.zoomIn) zoomFactor += 0.01f;
    if (input.zoomOut) zoomFactor -= 0.01f;

    // NOTE(jan): Calculate uniforms (projection matrix &c).
    Uniforms uniforms;

    matrixOrtho(windowWidth, windowHeight, uniforms.ortho);
    matrixOrthoCenteredOrigin(windowWidth, windowHeight, uniforms.orthoSociogram);

    matrixInit(uniforms.proj);
    matrix2DZoom(zoomFactor, uniforms.proj);

    updateUniforms(vk, &uniforms, sizeof(Uniforms));

    // NOTE(jan): Meshes are cleared and recalculated each frame.
    for (auto& pair: renderer.meshes) {
        Mesh& mesh = pair.second;
        mesh.indexCount = 0;
        mesh.indices.clear();
        mesh.vertexCount = 0;
        mesh.vertices.clear();
    }

    RENDERER_GET(boxes, meshes, "boxes");
    RENDERER_GET(consoleMesh, meshes, "console");
    RENDERER_GET(lines, meshes, "lines");
    RENDERER_GET(text, meshes, "text");
    RENDERER_GET(hud_text, meshes, "hud_text");
    RENDERER_GET(font, fonts, "default");

    std::vector<VulkanMesh> meshesToFree;

    // NOTE(jan): HUD
    {
        AABox hudBox = {
            .x0 = 0,
            .x1 = windowWidth,
            .y1 = windowHeight,
        };

        char hud[1 * KIBIBYTE];
        sprintf(hud, "%llu frames, %fs", frameCount, getElapsed());
        hudBox = pushText(text, font, hudBox, stringLiteral(hud), base01);
    }

    // NOTE(jan): Sociogram
    if (nodes.size() > 0) {
        AABox bbox = {
            .x0 = nodes[0].x,
            .x1 = nodes[0].x,
            .y0 = nodes[0].y,
            .y1 = nodes[0].y
        };

        for (const Vec2& node: nodes) {
            bbox.x0 = fmin(bbox.x0, node.x);
            bbox.x1 = fmax(bbox.x1, node.x);
            bbox.y0 = fmin(bbox.y0, node.y);
            bbox.y1 = fmax(bbox.y1, node.y);
        }

        f32 start = getElapsed();
        struct quad_node* root = quad_tree_build(bbox, &frameArena);
        f32 end = getElapsed();

        if (print_stats) {
            INFO("%d nodes in tree, CoM is (%f %f)", root->count, root->center.x, root->center.y);

            const f32 used = getMemoryArenaUsed(&frameArena);
            const f32 kb_used = used / 1024.f;
            const f32 mb_used = kb_used / 1024.f;

            const f32 free = getMemoryArenaFree(&frameArena);
            const f32 kb_free = free / 1024.f;
            const f32 mb_free = kb_free / 1024.f;

            const f32 kb_size = frameArena.size / 1024.f;
            const f32 mb_size = kb_size         / 1024.f;
            INFO("%d nodes, %.3fms, %f MiB used of %f MiB (%f MiB free)", nodes.size(), (end-start)*1000, mb_used, mb_size, mb_free);

            print_stats = false;
        }

        // NOTE(jan): Draw
        if (nodes.size() <= 100000) {
            quad_tree_draw(renderer, root, bbox);

            for (const Vec2& node: nodes) {
                bbox.x0 = fmin(bbox.x0, node.x);
                bbox.x1 = fmax(bbox.x1, node.x);
                bbox.y0 = fmin(bbox.y0, node.y);
                bbox.y1 = fmax(bbox.y1, node.y);

                AABox box = {
                    .x0 = node.x - 2.f,
                    .x1 = node.x + 2.f,
                    .y0 = node.y - 2.f,
                    .y1 = node.y + 2.f
                };
                pushAABox(boxes, box, magenta);
            }

            for (const edge& edge: edges) {
                Vec2 from = nodes[edge.from];
                Vec2 to = nodes[edge.to];
                pushLine(lines, from, to, magenta);
            }
        }

        const Vec2 center = { 0.f, 0.f };
        for (Vec2& node: nodes) {
            Vec2 gravity = { 0.f, 0.f };
            vectorSub(center, node, gravity);
            vectorScale(0.01f, gravity);

            Vec2 repulsion = quad_tree_update_node(root, bbox, node);

            vectorAdd(node, gravity, node);
            vectorAdd(node, repulsion, node);
        }

        for (edge& edge: edges) {
            Vec2& from_node = nodes[edge.from];
            Vec2& to_node = nodes[edge.to];

            Vec2 v;
            vectorSub(to_node, from_node, v);

            Vec2 from_delta = v;
            Vec2 to_delta = v;

            vectorScale( 0.01f, from_delta);
            vectorScale(-0.01f, to_delta);

            vectorAdd(from_node, from_delta, from_node);
            vectorAdd(to_node, to_delta, to_node);
        }
    }

    if (input.consoleToggle) {
        console.show = !console.show;
        input.consoleToggle = false;
    }
    if (input.consoleNewLine) {
        if (input.cmd[0] == 'I') {
            string num = input.cmd.substr(1);
            umm d = 0;
            sscanf_s(num.c_str(), "%lu", &d);
            node_insert_random(d);
        } else if (input.cmd[0] == 'C') {
            nodes.clear();
        }
        string s = "> " + input.cmd;
        logRaw(s.c_str());
        input.cmd = "";
        input.consoleNewLine = false;
    }

    if (console.show) {
        // NOTE(jan): Building mesh for console.
        AABox backgroundBox = {
            .x0 = 0.f,
            .x1 = windowWidth,
            .y0 = 0.f,
            .y1 = windowHeight / 2.f
        };
        pushAABox(consoleMesh, backgroundBox, base02);

        // NOTE(jan): Building mesh for console prompt.
        const f32 margin = font.info.size / 2.f;
        const f32 console_height = backgroundBox.y1 - backgroundBox.y0 - margin;
        const u32 console_line_height = console_height / font.info.size;

        if (input.consolePageUp && (console.lines.viewOffset < console.lines.count - console_line_height)) {
            console.lines.viewOffset++;
        }
        if (input.consolePageDown && (console.lines.viewOffset > 0)) {
            console.lines.viewOffset--;
        }

        AABox consoleLineBox = {
            .x0 = margin,
            .x1 = backgroundBox.x1,
            .y1 = backgroundBox.y1 - margin,
        };
        string s = "> " + input.cmd;
        AABox promptBox = pushText(text, font, consoleLineBox, stringLiteral(s.c_str()), base01);

        f32 cursorAlpha = (1 + sin(frameStart * 10.f)) / 2.f;
        AABox cursorBox = {};
        cursorBox.x0 = promptBox.x1;
        cursorBox.x1 = backgroundBox.x1;
        cursorBox.y1 = promptBox.y1;
        Vec4 cursorColor = {
            .x = base01.x,
            .y = base01.y,
            .z = base01.z,
            .w = cursorAlpha
        };
        cursorBox = pushText(text, font, cursorBox, stringLiteral("_"), cursorColor);

        consoleLineBox.y1 = cursorBox.y0;

        // NOTE(jan): Building mesh for console scrollback.
        umm top = console.lines.next >= 1 + console.lines.viewOffset ? console.lines.next : console.lines.count;
        umm lineIndex = top - 1 - console.lines.viewOffset;
        for (umm i = 0; i < console.lines.count; i++) {
            if (consoleLineBox.y1 < 0) break;
            ConsoleLine line = console.lines.data[lineIndex];
            String consoleText = {
                .size = line.size,
                .length = line.size,
                .data = (char*)console.data + line.start,
            };
            AABox prevLineBox = pushText(text, font, consoleLineBox, consoleText, base01);
            consoleLineBox.y1 = prevLineBox.y0;

            if (lineIndex > 0) {
                lineIndex--;
            } else {
                lineIndex = console.lines.count - 1;
            }
        }
    }

    // NOTE(jan): Update uniforms.
    for (auto kv: renderer.pipelines) {
        auto& pipeline = kv.second;
        updateUniformBuffer(vk.device, pipeline.descriptorSet, 0, vk.uniforms.handle);

        const char* key = kv.first;
        if (font.sampler.handle != VK_NULL_HANDLE) {
            updateCombinedImageSampler(
                vk.device, pipeline.descriptorSet, 1, &font.sampler, 1
            );
        }
    }

    // NOTE(jan): Start recording commands.
    VkCommandBuffer cmds = {};
    createCommandBuffers(vk.device, vk.cmdPool, 1, &cmds);
    beginFrameCommandBuffer(cmds);

    // NOTE(jan): Clear colour / depth.
    VkClearValue colorClear;
    colorClear.color.float32[0] = 0.f;
    colorClear.color.float32[1] = 0.f;
    colorClear.color.float32[2] = 0.f;
    colorClear.color.float32[3] = 1.f;
    VkClearValue depthClear;
    depthClear.depthStencil = {1.f, 0};
    VkClearValue clears[] = {colorClear, depthClear};

    // NOTE(jan): Render pass.
    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.clearValueCount = 2;
    beginInfo.pClearValues = clears;
    beginInfo.framebuffer = vk.swap.framebuffers[swapImageIndex];
    beginInfo.renderArea.extent = vk.swap.extent;
    beginInfo.renderArea.offset = {0, 0};
    beginInfo.renderPass = vk.renderPass;

    vkCmdBeginRenderPass(cmds, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    const char* brushOrder[] = {
        "boxes",
        "lines",
        "hud_text",
        "console",
        "text",
    };
    for (auto key: brushOrder) {
        RENDERER_GET(brush, brushes, key);

        RENDERER_GET(pipeline, pipelines, brush.info.pipelineName);
        vkCmdBindPipeline(
            cmds, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle
        );
        vkCmdBindDescriptorSets(
            cmds, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout,
            0, 1, &pipeline.descriptorSet,
            0, nullptr
        );

        RENDERER_GET(mesh, meshes, brush.info.meshName);
        if ((mesh.indexCount == 0) || (mesh.vertexCount == 0)) continue;

        VulkanMesh& vkMesh = meshesToFree.emplace_back();
        uploadMesh(
            vk,
            mesh.vertices.data(), sizeof(mesh.vertices[0]) * mesh.vertices.size(),
            mesh.indices.data(), sizeof(mesh.indices[0]) * mesh.indices.size(),
            vkMesh
        );
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmds, 0, 1, &vkMesh.vBuff.handle, offsets);
        vkCmdBindIndexBuffer(cmds, vkMesh.iBuff.handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmds, mesh.indices.size(), 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmds);
    endCommandBuffer(cmds);

    // Submit.
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmds;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &vk.swap.imageReady;
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
    };
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &vk.swap.cmdBufferDone;
    vkQueueSubmit(vk.queue, 1, &submitInfo, VK_NULL_HANDLE);

    // Present.
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vk.swap.handle;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &vk.swap.cmdBufferDone;
    presentInfo.pImageIndices = &swapImageIndex;
    VKCHECK(vkQueuePresentKHR(vk.queue, &presentInfo))

    // PERF(jan): This is potentially slow.
    vkQueueWaitIdle(vk.queue);

    // Cleanup.
    for (auto& mesh: meshesToFree) {
        destroyMesh(vk, mesh);
    }
    if (font.isDirty) packFont(font);

    memoryArenaClear(&frameArena);

    frameCount++;
}

// ************************************************************
// * INIT: Everything required to set up Vulkan pipelines &c. *
// ************************************************************

void init(Vulkan& vk, Renderer& renderer) {
    for (const FontInfo& info: fontInfo) {
        INFO("Loading font '%s'...", info.name);

        Font font = {
            .info = info,
            .ttfFileContents = readFile(info.path),
        };

        RENDERER_PUT(font, fonts, info.name);
    }

    for (const MeshInfo& info: meshInfo) {
        INFO("Creating mesh '%s'...", info.name);

        Mesh mesh = {
            .info = info,
            // TODO(jan): Calculate this from mesh metadata.
            .vertexSizeInFloats = 8
        };

        RENDERER_PUT(mesh, meshes, info.name);
    }

    for (const PipelineInfo& info: pipelineInfo) {
        INFO("Creating pipeline '%s'...", info.name);

        VulkanPipeline pipeline = {};
        initVKPipeline(vk, info, pipeline);

        RENDERER_PUT(pipeline, pipelines, info.name);
    }

    for (const BrushInfo& info: brushInfo) {
        INFO("Creating brush '%s'...", info.name);

        Brush brush = {
            .info = info,
        };

        renderer.brushes.insert({ info.name, brush });
    }
}

// **********************************
// * WIN32: Windows specific stuff. *
// **********************************

LRESULT __stdcall
WindowProc(
    HWND    window,
    UINT    message,
    WPARAM  wParam,
    LPARAM  lParam
) {
    switch (message) {
        case WM_DESTROY: {
            PostQuitMessage(0);
            break;
        } case WM_KEYDOWN: {
            BOOL repeatFlag = (HIWORD(lParam) & KF_REPEAT) == KF_REPEAT;
            switch (wParam) {
                case VK_ESCAPE: PostQuitMessage(0); break;
                case VK_PRIOR: input.consolePageUp = true; break;
                case VK_NEXT: input.consolePageDown = true; break;
                case VK_RETURN: input.consoleNewLine = true; break;
                case VK_F1: input.consoleToggle = true; break;
                case 'I': if (!console.show) node_insert_random(1); break;
                case 'Z': if (!console.show) input.zoomIn = true; break;
                case 'X': if (!console.show) input.zoomOut = true; break;
            }
            if (console.show) {
                if (
                    ((wParam >= 0x30) && (wParam <= 0x39)) ||
                    ((wParam >= 0x41) && (wParam <= 0x5A))
                ) {
                    input.cmd += (char)wParam;
                }
            }
            break;
        } case WM_KEYUP: {
            switch (wParam) {
                case VK_F1: input.consoleToggle = false; break;
                case VK_PRIOR: input.consolePageUp = false; break;
                case VK_NEXT: input.consolePageDown = false; break;
                case 'Z': if (!console.show) input.zoomIn = false; break;
                case 'X': if (!console.show) input.zoomOut = false; break;
            }
            break;
        } default: {
            return DefWindowProc(window, message, wParam, lParam);
        }
    }
    return 0;
}

int __stdcall
WinMain(
    HINSTANCE instance,
    HINSTANCE prevInstance,
    LPSTR commandLine,
    int showCommand
) {
    auto error = fopen_s(&logFile, "LOG", "w");
    if (error) exit(-1);
    console = initConsole(1 * 1024 * 1024);

    QueryPerformanceCounter(&counterEpoch);
    QueryPerformanceFrequency(&counterFrequency);
    INFO("Logging initialized.");

    // Create Window.
    WNDCLASSEX windowClassProperties = {};
    windowClassProperties.cbSize = sizeof(windowClassProperties);
    windowClassProperties.style = CS_HREDRAW | CS_VREDRAW;
    windowClassProperties.lpfnWndProc = (WNDPROC)WindowProc;
    windowClassProperties.hInstance = instance;
    windowClassProperties.lpszClassName = "MainWindowClass";
    ATOM windowClass = RegisterClassEx(&windowClassProperties);
    if (!windowClass) {
        FATAL("could not create window class")
    }
    HWND window = CreateWindowEx(
        0,
        "MainWindowClass",
        "Vk",
        WS_POPUP | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        WIDTH,
        HEIGHT,
        nullptr,
        nullptr,
        instance,
        nullptr
    );
    if (window == nullptr) {
        FATAL("could not create window")
    }
    SetWindowPos(
        window,
        HWND_TOP,
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        SWP_FRAMECHANGED
    );
    ShowCursor(FALSE);

    // TODO(jan): Handle resize.
    GetWindowRect(window, &windowRect);
    windowWidth = windowRect.right - windowRect.left;
    windowHeight = windowRect.bottom - windowRect.top;

    // NOTE(jan): Create Vulkan instance..
    vk.extensions.emplace_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    createVKInstance(vk);

    // NOTE(jan): Get a surface for Vulkan.
    {
        VkSurfaceKHR surface;

        VkWin32SurfaceCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hinstance = instance;
        createInfo.hwnd = window;

        auto result = vkCreateWin32SurfaceKHR(
            vk.handle,
            &createInfo,
            nullptr,
            &surface
        );

        if (result != VK_SUCCESS) {
            throw runtime_error("could not create win32 surface");
        }
        vk.swap.surface = surface;
    }

    // Initialize the rest of Vulkan.
    initVK(vk);

    // Load shaders, meshes, fonts, textures, and other resources.
    Renderer renderer;
    init(vk, renderer);

    // Load data.
    // load_sociogram();
    map<umm, umm> id_index_map;

    {
        FILE* sociogram_file = openFile("F:/links-anon.txt", "r");

        const auto bufferSize = 50 * KIBIBYTE;
        auto buffer = new char[bufferSize];
        const auto readCount = readFromFile(sociogram_file, bufferSize, buffer);

        char* bufferBegin = buffer;
        char* bufferEnd = buffer + bufferSize;
        char* fromStart = bufferBegin;
        char* fromEnd = 0;
        char* toStart = 0;
        char* toEnd = 0;
        u8 state = 0;
        for (char* c = bufferBegin; c < bufferEnd; c++) {
            if ((state == 0) && isdigit(*c)) {
                state = 1;
                fromStart = c;
                fromEnd = c;
            } else if (state == 1) {
                if (isdigit(*c)) {
                    fromEnd = c;
                } else if (*c == ' ') {
                    state = 2;
                }
            } else if ((state == 2) && isdigit(*c)) {
                state = 3;
                toStart = c;
                toEnd = c;
            } else if (state == 3) {
                if (isdigit(*c)) {
                    toEnd = c;
                } else if (*c == '\n') {
                    state = 0;

                    u64 from_id = 0;
                    u16 pow = 1;
                    for (char* d = fromEnd; d >= fromStart; d--) {
                        u8 digit = *d - 48;
                        from_id += digit * pow;
                        pow *= 10;
                    }

                    if (!id_index_map.contains(from_id)) {
                        node_insert_random(1);
                        id_index_map[from_id] = nodes.size() - 1;
                    }

                    u64 to_id = 0;
                    pow = 1;
                    for (char* d = toEnd; d >= toStart; d--) {
                        u8 digit = *d - 48;
                        to_id += digit * pow;
                        pow *= 10;
                    }

                    if (!id_index_map.contains(to_id)) {
                        node_insert_random(1);
                        id_index_map[to_id] = nodes.size() - 1;
                    }

                    struct edge& edge = edges.emplace_back();
                    edge.from = id_index_map[from_id];
                    edge.to = id_index_map[to_id];
                }
            }
        }
    }

    for (auto& edge: edges) {
        INFO("%d -> %d", edge.from, edge.to);
    }

    // NOTE(jan): Main loop.
    bool done = false;
    while (!done) {
        // NOTE(jan): Pump WIN32 message queue.
        MSG msg;
        BOOL messageAvailable;
        while(true) {
            messageAvailable = PeekMessage(
                &msg,
                (HWND)nullptr,
                0, 0,
                PM_REMOVE
            );
            if (!messageAvailable) break;
            TranslateMessage(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
            DispatchMessage(&msg);
        }

        doFrame(vk, renderer);
    }

    return 0;
}
