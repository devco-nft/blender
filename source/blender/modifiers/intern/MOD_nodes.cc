/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup modifiers
 */

#include <cstring>
#include <iostream>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_float3.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_customdata.h"
#include "BKE_idprop.h"
#include "BKE_lib_query.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_pointcloud.h"
#include "BKE_screen.h"
#include "BKE_simulation.h"

#include "BLO_read_write.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"

#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "MOD_modifiertypes.h"
#include "MOD_nodes.h"
#include "MOD_ui_common.h"

#include "NOD_derived_node_tree.hh"
#include "NOD_geometry_exec.hh"
#include "NOD_node_tree_multi_function.hh"
#include "NOD_type_callbacks.hh"

using blender::float3;

static void initData(ModifierData *md)
{
  NodesModifierData *nmd = (NodesModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(nmd, modifier));

  MEMCPY_STRUCT_AFTER(nmd, DNA_struct_default_get(NodesModifierData), modifier);
}

static void updateDepsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  if (nmd->node_group != nullptr) {
    DEG_add_node_tree_relation(ctx->node, nmd->node_group, "Nodes Modifier");
  }

  /* TODO: Add relations for IDs in settings. */
}

static void foreachIDLink(ModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  walk(userData, ob, (ID **)&nmd->node_group, IDWALK_CB_USER);

  struct ForeachSettingData {
    IDWalkFunc walk;
    void *userData;
    Object *ob;
  } settings = {walk, userData, ob};

  IDP_foreach_property(
      nmd->settings.properties,
      IDP_TYPE_FILTER_ID,
      [](IDProperty *id_prop, void *user_data) {
        ForeachSettingData *settings = (ForeachSettingData *)user_data;
        settings->walk(
            settings->userData, settings->ob, (ID **)&id_prop->data.pointer, IDWALK_CB_USER);
      },
      &settings);
}

static bool isDisabled(const struct Scene *UNUSED(scene),
                       ModifierData *md,
                       bool UNUSED(useRenderParams))
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  UNUSED_VARS(nmd);
  return false;
}

static PointCloud *modifyPointCloud(ModifierData *md,
                                    const ModifierEvalContext *UNUSED(ctx),
                                    PointCloud *pointcloud)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  UNUSED_VARS(nmd);
  std::cout << __func__ << "\n";
  return pointcloud;
}

/* To be replaced soon. */
using namespace blender;
using namespace blender::nodes;
using namespace blender::fn;
using namespace blender::bke;

class GeometryNodesEvaluator {
 private:
  LinearAllocator<> allocator_;
  Map<const DInputSocket *, GMutablePointer> value_by_input_;
  Vector<const DInputSocket *> group_outputs_;
  MultiFunctionByNode &mf_by_node_;
  const DataTypeConversions &conversions_;

 public:
  GeometryNodesEvaluator(const Map<const DOutputSocket *, GMutablePointer> &group_input_data,
                         Vector<const DInputSocket *> group_outputs,
                         MultiFunctionByNode &mf_by_node)
      : group_outputs_(std::move(group_outputs)),
        mf_by_node_(mf_by_node),
        conversions_(get_implicit_type_conversions())
  {
    for (auto item : group_input_data.items()) {
      this->forward_to_inputs(*item.key, item.value);
    }
  }

  Vector<GMutablePointer> execute()
  {
    Vector<GMutablePointer> results;
    for (const DInputSocket *group_output : group_outputs_) {
      GMutablePointer result = this->get_input_value(*group_output);
      results.append(result);
    }
    for (GMutablePointer value : value_by_input_.values()) {
      value.destruct();
    }
    return results;
  }

 private:
  GMutablePointer get_input_value(const DInputSocket &socket_to_compute)
  {
    std::optional<GMutablePointer> value = value_by_input_.pop_try(&socket_to_compute);
    if (value.has_value()) {
      /* This input has been computed before, return it directly. */
      return *value;
    }

    Span<const DOutputSocket *> from_sockets = socket_to_compute.linked_sockets();
    Span<const DGroupInput *> from_group_inputs = socket_to_compute.linked_group_inputs();
    const int total_inputs = from_sockets.size() + from_group_inputs.size();
    BLI_assert(total_inputs <= 1);

    const CPPType &type = *socket_cpp_type_get(*socket_to_compute.typeinfo());

    if (total_inputs == 0) {
      /* The input is not connected, use the value from the socket itself. */
      bNodeSocket &bsocket = *socket_to_compute.bsocket();
      void *buffer = allocator_.allocate(type.size(), type.alignment());
      socket_cpp_value_get(bsocket, buffer);
      return GMutablePointer{type, buffer};
    }
    if (from_group_inputs.size() == 1) {
      /* The input gets its value from the input of a group that is not further connected. */
      bNodeSocket &bsocket = *from_group_inputs[0]->bsocket();
      void *buffer = allocator_.allocate(type.size(), type.alignment());
      socket_cpp_value_get(bsocket, buffer);
      return GMutablePointer{type, buffer};
    }

    /* Compute the socket now. */
    const DOutputSocket &from_socket = *from_sockets[0];
    this->compute_output_and_forward(from_socket);
    return value_by_input_.pop(&socket_to_compute);
  }

  void compute_output_and_forward(const DOutputSocket &socket_to_compute)
  {
    const DNode &node = socket_to_compute.node();
    const bNode &bnode = *node.bnode();

    /* Prepare inputs required to execute the node. */
    GValueMap<StringRef> node_inputs_map{allocator_};
    for (const DInputSocket *input_socket : node.inputs()) {
      if (input_socket->is_available()) {
        GMutablePointer value = this->get_input_value(*input_socket);
        node_inputs_map.add_new_direct(input_socket->identifier(), value);
      }
    }

    /* Execute the node. */
    GValueMap<StringRef> node_outputs_map{allocator_};
    GeoNodeInputs node_inputs{bnode, node_inputs_map};
    GeoNodeOutputs node_outputs{bnode, node_outputs_map};
    this->execute_node(node, node_inputs, node_outputs);

    /* Forward computed outputs to linked input sockets. */
    for (const DOutputSocket *output_socket : node.outputs()) {
      if (output_socket->is_available()) {
        GMutablePointer value = node_outputs_map.extract(output_socket->identifier());
        this->forward_to_inputs(*output_socket, value);
      }
    }
  }

  void execute_node(const DNode &node, GeoNodeInputs node_inputs, GeoNodeOutputs node_outputs)
  {
    bNode *bnode = node.bnode();
    if (bnode->typeinfo->geometry_node_execute != nullptr) {
      bnode->typeinfo->geometry_node_execute(bnode, node_inputs, node_outputs);
      return;
    }

    /* Use the multi-function implementation of the node. */
    const MultiFunction &fn = *mf_by_node_.lookup(&node);
    MFContextBuilder context;
    MFParamsBuilder params{fn, 1};
    Vector<GMutablePointer> input_data;
    for (const DInputSocket *dsocket : node.inputs()) {
      if (dsocket->is_available()) {
        GMutablePointer data = node_inputs.extract(dsocket->identifier());
        params.add_readonly_single_input(GSpan(*data.type(), data.get(), 1));
        input_data.append(data);
      }
    }
    Vector<GMutablePointer> output_data;
    for (const DOutputSocket *dsocket : node.outputs()) {
      if (dsocket->is_available()) {
        const CPPType &type = *socket_cpp_type_get(*dsocket->typeinfo());
        void *buffer = allocator_.allocate(type.size(), type.alignment());
        params.add_uninitialized_single_output(GMutableSpan(type, buffer, 1));
        output_data.append(GMutablePointer(type, buffer));
      }
    }
    fn.call(IndexRange(1), params, context);
    for (GMutablePointer value : input_data) {
      value.destruct();
    }
    for (const int i : node.outputs().index_range()) {
      GMutablePointer value = output_data[i];
      node_outputs.set_by_move(node.output(i).identifier(), value);
      value.destruct();
    }
  }

  void forward_to_inputs(const DOutputSocket &from_socket, GMutablePointer value_to_forward)
  {
    Span<const DInputSocket *> to_sockets_all = from_socket.linked_sockets();

    const CPPType &from_type = *value_to_forward.type();

    Vector<const DInputSocket *> to_sockets_same_type;
    for (const DInputSocket *to_socket : to_sockets_all) {
      const CPPType &to_type = *socket_cpp_type_get(*to_socket->typeinfo());
      if (from_type == to_type) {
        to_sockets_same_type.append(to_socket);
      }
      else {
        void *buffer = allocator_.allocate(to_type.size(), to_type.alignment());
        if (conversions_.is_convertible(from_type, to_type)) {
          conversions_.convert(from_type, to_type, value_to_forward.get(), buffer);
        }
        else {
          to_type.copy_to_uninitialized(to_type.default_value(), buffer);
        }
        value_by_input_.add_new(to_socket, GMutablePointer{to_type, buffer});
      }
    }

    if (to_sockets_same_type.size() == 0) {
      /* This value is not further used, so destruct it. */
      value_to_forward.destruct();
    }
    else if (to_sockets_same_type.size() == 1) {
      /* This value is only used on one input socket, no need to copy it. */
      const DInputSocket *to_socket = to_sockets_same_type[0];
      value_by_input_.add_new(to_socket, value_to_forward);
    }
    else {
      /* Multiple inputs use the value, make a copy for every input except for one. */
      const DInputSocket *first_to_socket = to_sockets_same_type[0];
      Span<const DInputSocket *> other_to_sockets = to_sockets_same_type.as_span().drop_front(1);
      const CPPType &type = *value_to_forward.type();

      value_by_input_.add_new(first_to_socket, value_to_forward);
      for (const DInputSocket *to_socket : other_to_sockets) {
        void *buffer = allocator_.allocate(type.size(), type.alignment());
        type.copy_to_uninitialized(value_to_forward.get(), buffer);
        value_by_input_.add_new(to_socket, GMutablePointer{type, buffer});
      }
    }
  }
};

struct SocketPropertyType {
  IDProperty *(*create)(const bNodeSocket &socket, const char *name);
  bool (*is_correct_type)(const IDProperty &property);
  void (*init_cpp_value)(const IDProperty &property, void *r_value);
};

static const SocketPropertyType *get_socket_property_type(const bNodeSocket &bsocket)
{
  switch (bsocket.type) {
    case SOCK_FLOAT: {
      static const SocketPropertyType float_type = {
          [](const bNodeSocket &socket, const char *name) {
            IDPropertyTemplate value = {0};
            value.f = ((bNodeSocketValueFloat *)socket.default_value)->value;
            return IDP_New(IDP_FLOAT, &value, name);
          },
          [](const IDProperty &property) { return property.type == IDP_FLOAT; },
          [](const IDProperty &property, void *r_value) {
            *(float *)r_value = IDP_Float(&property);
          },
      };
      return &float_type;
    }
    case SOCK_INT: {
      static const SocketPropertyType int_type = {
          [](const bNodeSocket &socket, const char *name) {
            IDPropertyTemplate value = {0};
            value.i = ((bNodeSocketValueInt *)socket.default_value)->value;
            return IDP_New(IDP_INT, &value, name);
          },
          [](const IDProperty &property) { return property.type == IDP_INT; },
          [](const IDProperty &property, void *r_value) { *(int *)r_value = IDP_Int(&property); },
      };
      return &int_type;
    }
    case SOCK_VECTOR: {
      static const SocketPropertyType vector_type = {
          [](const bNodeSocket &socket, const char *name) {
            IDPropertyTemplate value = {0};
            value.array.len = 3;
            value.array.type = IDP_FLOAT;
            IDProperty *property = IDP_New(IDP_ARRAY, &value, name);
            copy_v3_v3((float *)IDP_Array(property),
                       ((bNodeSocketValueVector *)socket.default_value)->value);
            return property;
          },
          [](const IDProperty &property) {
            return property.type == IDP_ARRAY && property.subtype == IDP_FLOAT &&
                   property.len == 3;
          },
          [](const IDProperty &property, void *r_value) {
            copy_v3_v3((float *)r_value, (const float *)IDP_Array(&property));
          },
      };
      return &vector_type;
    }
    case SOCK_BOOLEAN: {
      static const SocketPropertyType boolean_type = {
          [](const bNodeSocket &socket, const char *name) {
            IDPropertyTemplate value = {0};
            value.i = ((bNodeSocketValueBoolean *)socket.default_value)->value != 0;
            return IDP_New(IDP_INT, &value, name);
          },
          [](const IDProperty &property) { return property.type == IDP_INT; },
          [](const IDProperty &property, void *r_value) {
            *(bool *)r_value = IDP_Int(&property) != 0;
          },
      };
      return &boolean_type;
    }
    default: {
      return nullptr;
    }
  }
}

void MOD_nodes_update_interface(Object *object, NodesModifierData *nmd)
{
  if (nmd->node_group == nullptr) {
    return;
  }
  if (nmd->settings.properties == nullptr) {
    IDPropertyTemplate default_value = {0};
    nmd->settings.properties = IDP_New(IDP_GROUP, &default_value, "Nodes Modifier Settings");
  }

  LISTBASE_FOREACH (bNodeSocket *, socket, &nmd->node_group->inputs) {
    const char *identifier = socket->identifier;
    const SocketPropertyType *property_type = get_socket_property_type(*socket);
    if (property_type == nullptr) {
      continue;
    }

    IDProperty *property = IDP_GetPropertyFromGroup(nmd->settings.properties, identifier);
    if (property == nullptr) {
      IDProperty *new_property = property_type->create(*socket, socket->identifier);
      IDP_AddToGroup(nmd->settings.properties, new_property);
    }
    else if (!property_type->is_correct_type(*property)) {
      IDP_FreeFromGroup(nmd->settings.properties, property);
      property = property_type->create(*socket, socket->identifier);
      IDP_AddToGroup(nmd->settings.properties, property);
    }
  }

  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
}

static void initialize_group_input(NodesModifierData &nmd,
                                   const bNodeSocket &socket,
                                   const CPPType &cpp_type,
                                   void *r_value)
{
  const SocketPropertyType *property_type = get_socket_property_type(socket);
  if (property_type == nullptr) {
    cpp_type.copy_to_uninitialized(cpp_type.default_value(), r_value);
    return;
  }
  if (nmd.settings.properties == nullptr) {
    socket_cpp_value_get(socket, r_value);
    return;
  }
  const IDProperty *property = IDP_GetPropertyFromGroup(nmd.settings.properties,
                                                        socket.identifier);
  if (property == nullptr) {
    socket_cpp_value_get(socket, r_value);
    return;
  }
  if (!property_type->is_correct_type(*property)) {
    socket_cpp_value_get(socket, r_value);
  }
  property_type->init_cpp_value(*property, r_value);
}

/**
 * Evaluate a node group to compute the output geometry.
 * Currently, this uses a fairly basic and inefficient algorithm that might compute things more
 * often than necessary. It's going to be replaced soon.
 */
static GeometryPtr compute_geometry(const DerivedNodeTree &tree,
                                    Span<const DOutputSocket *> group_input_sockets,
                                    const DInputSocket &socket_to_compute,
                                    GeometryPtr input_geometry,
                                    NodesModifierData *nmd)
{
  ResourceCollector resources;
  LinearAllocator<> &allocator = resources.linear_allocator();
  MultiFunctionByNode mf_by_node = get_multi_function_per_node(tree, resources);

  Map<const DOutputSocket *, GMutablePointer> group_inputs;

  if (group_input_sockets.size() > 0) {
    Span<const DOutputSocket *> remaining_input_sockets = group_input_sockets;

    /* If the group expects a geometry as first input, use the geometry that has been passed to
     * modifier. */
    const DOutputSocket *first_input_socket = group_input_sockets[0];
    if (first_input_socket->bsocket()->type == SOCK_GEOMETRY) {
      GeometryPtr *geometry_in = allocator.construct<GeometryPtr>(std::move(input_geometry));
      group_inputs.add_new(first_input_socket, geometry_in);
      remaining_input_sockets = remaining_input_sockets.drop_front(1);
    }

    /* Initialize remaining group inputs. */
    for (const DOutputSocket *socket : remaining_input_sockets) {
      const CPPType &cpp_type = *socket_cpp_type_get(*socket->typeinfo());
      void *value_in = allocator.allocate(cpp_type.size(), cpp_type.alignment());
      initialize_group_input(*nmd, *socket->bsocket(), cpp_type, value_in);
      group_inputs.add_new(socket, {cpp_type, value_in});
    }
  }

  Vector<const DInputSocket *> group_outputs;
  group_outputs.append(&socket_to_compute);

  GeometryNodesEvaluator evaluator{group_inputs, group_outputs, mf_by_node};
  Vector<GMutablePointer> results = evaluator.execute();
  BLI_assert(results.size() == 1);
  GMutablePointer result = results[0];

  GeometryPtr output_geometry = std::move(*(GeometryPtr *)result.get());
  return output_geometry;
}

static Mesh *modifyMesh(ModifierData *md, const ModifierEvalContext *UNUSED(ctx), Mesh *mesh)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  if (nmd->node_group == nullptr) {
    return mesh;
  }

  NodeTreeRefMap tree_refs;
  DerivedNodeTree tree{nmd->node_group, tree_refs};
  ResourceCollector resources;

  Span<const DNode *> input_nodes = tree.nodes_by_type("NodeGroupInput");
  Span<const DNode *> output_nodes = tree.nodes_by_type("NodeGroupOutput");

  if (input_nodes.size() > 1) {
    return mesh;
  }
  if (output_nodes.size() != 1) {
    return mesh;
  }

  Span<const DOutputSocket *> group_inputs = (input_nodes.size() == 1) ?
                                                 input_nodes[0]->outputs().drop_back(1) :
                                                 Span<const DOutputSocket *>{};
  Span<const DInputSocket *> group_outputs = output_nodes[0]->inputs().drop_back(1);

  if (group_outputs.size() != 1) {
    return mesh;
  }

  const DInputSocket *group_output = group_outputs[0];
  if (group_output->idname() != "NodeSocketGeometry") {
    return mesh;
  }

  GeometryPtr input_geometry{Geometry::create_with_mesh(mesh, false)};

  GeometryPtr new_geometry = compute_geometry(
      tree, group_inputs, *group_outputs[0], std::move(input_geometry), nmd);
  make_geometry_mutable(new_geometry);
  Mesh *new_mesh = new_geometry->get_component_for_write<MeshComponent>().release();
  if (new_mesh == nullptr) {
    return BKE_mesh_new_nomain(0, 0, 0, 0, 0);
  }
  return new_mesh;
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
#ifdef WITH_GEOMETRY_NODES
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  NodesModifierData *nmd = static_cast<NodesModifierData *>(ptr->data);

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  uiItemR(layout, ptr, "node_group", 0, NULL, ICON_MESH_DATA);

  if (nmd->node_group != nullptr && nmd->settings.properties != nullptr) {
    PointerRNA settings_ptr;
    RNA_pointer_create(ptr->owner_id, &RNA_NodesModifierSettings, &nmd->settings, &settings_ptr);
    LISTBASE_FOREACH (bNodeSocket *, socket, &nmd->node_group->inputs) {
      const SocketPropertyType *property_type = get_socket_property_type(*socket);
      if (property_type == nullptr) {
        continue;
      }
      IDProperty *property = IDP_GetPropertyFromGroup(nmd->settings.properties,
                                                      socket->identifier);
      if (property != nullptr) {
        if (property_type->is_correct_type(*property)) {
          char rna_path[128];
          BLI_snprintf(rna_path, ARRAY_SIZE(rna_path), "[\"%s\"]", socket->identifier);
          uiItemR(layout, &settings_ptr, rna_path, 0, socket->name, ICON_NONE);
        }
      }
    }
  }

  modifier_panel_end(layout, ptr);
#endif
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Nodes, panel_draw);
}

static void blendWrite(BlendWriter *writer, const ModifierData *md)
{
  const NodesModifierData *nmd = reinterpret_cast<const NodesModifierData *>(md);
  if (nmd->settings.properties != nullptr) {
    IDP_BlendWrite(writer, nmd->settings.properties);
  }
}

static void blendRead(BlendDataReader *reader, ModifierData *md)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  BLO_read_data_address(reader, &nmd->settings.properties);
  IDP_BlendDataRead(reader, &nmd->settings.properties);
}

static void copyData(const ModifierData *md, ModifierData *target, const int flag)
{
  const NodesModifierData *nmd = reinterpret_cast<const NodesModifierData *>(md);
  NodesModifierData *tnmd = reinterpret_cast<NodesModifierData *>(target);

  BKE_modifier_copydata_generic(md, target, flag);

  if (nmd->settings.properties != nullptr) {
    tnmd->settings.properties = IDP_CopyProperty_ex(nmd->settings.properties, flag);
  }
}

static void freeData(ModifierData *md)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  if (nmd->settings.properties != nullptr) {
    IDP_FreeProperty_ex(nmd->settings.properties, false);
    nmd->settings.properties = nullptr;
  }
}

ModifierTypeInfo modifierType_Nodes = {
    /* name */ "Nodes",
    /* structName */ "NodesModifierData",
    /* structSize */ sizeof(NodesModifierData),
#ifdef WITH_GEOMETRY_NODES
    /* srna */ &RNA_NodesModifier,
#else
    /* srna */ &RNA_Modifier,
#endif
    /* type */ eModifierTypeType_Constructive,
    /* flags */
    static_cast<ModifierTypeFlag>(eModifierTypeFlag_AcceptsMesh |
                                  eModifierTypeFlag_SupportsEditmode |
                                  eModifierTypeFlag_EnableInEditmode),
    /* icon */ ICON_MESH_DATA, /* TODO: Use correct icon. */

    /* copyData */ copyData,

    /* deformVerts */ NULL,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ modifyMesh,
    /* modifyHair */ NULL,
    /* modifyPointCloud */ modifyPointCloud,
    /* modifyVolume */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ NULL,
    /* freeData */ freeData,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ NULL,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ blendWrite,
    /* blendRead */ blendRead,
};