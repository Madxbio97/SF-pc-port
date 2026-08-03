class GlassShatterPresentation final {
public:
  void reset(const game::GameplaySession &gameplay) {
    shards_.clear();
    destroyed_.resize(gameplay.objects().size());
    identities_.resize(gameplay.objects().size());
    for (std::size_t object = 0U; object < gameplay.objects().size();
         ++object) {
      destroyed_[object] =
          gameplay.objectDestroyed(static_cast<std::uint16_t>(object));
      identities_[object] = identity(gameplay.objects()[object]);
    }
  }

  void observe(const game::GameplaySession &gameplay,
               const RenderPresentationSnapshot &presentation) {
    if (destroyed_.size() != gameplay.objects().size()) {
      const auto previous_size = destroyed_.size();
      destroyed_.resize(gameplay.objects().size());
      identities_.resize(gameplay.objects().size());
      for (auto object = previous_size; object < gameplay.objects().size();
           ++object) {
        destroyed_[object] =
            gameplay.objectDestroyed(static_cast<std::uint16_t>(object));
        identities_[object] = identity(gameplay.objects()[object]);
      }
    }
    const auto count =
        std::min(gameplay.objects().size(), presentation.objects.size());
    for (std::size_t object = 0U; object < count; ++object) {
      const auto &scene_object = gameplay.objects()[object];
      const auto current_identity = identity(scene_object);
      const auto destroyed =
          gameplay.objectDestroyed(static_cast<std::uint16_t>(object));
      if (identities_[object] != current_identity) {
        identities_[object] = current_identity;
        destroyed_[object] = destroyed;
        continue;
      }
      if (destroyed && !destroyed_[object] &&
          scene_object.damage_response == game::ObjectDamageResponse::shatter) {
        spawn(gameplay, presentation, static_cast<std::uint16_t>(object));
      }
      destroyed_[object] = destroyed;
    }
  }

  void advance(unsigned int guest_ticks) {
    if (guest_ticks == 0U) {
      return;
    }
    for (auto &shard : shards_) {
      shard.age = static_cast<std::uint16_t>(std::min<unsigned int>(
          static_cast<unsigned int>(shard.age) + guest_ticks,
          std::numeric_limits<std::uint16_t>::max()));
    }
    std::erase_if(
        shards_, [](const auto &shard) { return shard.age >= shard.lifetime; });
  }

  [[nodiscard]] std::span<const GlassShardPresentationState>
  shards() const noexcept {
    return shards_;
  }

private:
  struct SourceVertex {
    Vector3 position;
    double u{};
    double v{};
  };

  [[nodiscard]] static std::uint64_t
  identity(const game::SceneObject &object) noexcept {
    return static_cast<std::uint64_t>(object.model) |
           (static_cast<std::uint64_t>(object.source_index) << 16U) |
           (static_cast<std::uint64_t>(object.class_id) << 32U);
  }

  [[nodiscard]] static SourceVertex midpoint(const SourceVertex &first,
                                             const SourceVertex &second) {
    return SourceVertex{
        Vector3{(first.position.x + second.position.x) * 0.5,
                (first.position.y + second.position.y) * 0.5,
                (first.position.z + second.position.z) * 0.5},
        (first.u + second.u) * 0.5,
        (first.v + second.v) * 0.5,
    };
  }

  void appendShard(const std::array<SourceVertex, 3U> &vertices,
                   const assets::GmdTriangle &material,
                   std::uint8_t texture_bank, const game::CameraState &camera,
                   std::uint32_t seed) {
    constexpr auto maximum_active_shards = std::size_t{256U};
    if (shards_.size() == maximum_active_shards) {
      shards_.erase(shards_.begin());
    }
    const auto centre = Vector3{
        (vertices[0].position.x + vertices[1].position.x +
         vertices[2].position.x) /
            3.0,
        (vertices[0].position.y + vertices[1].position.y +
         vertices[2].position.y) /
            3.0,
        (vertices[0].position.z + vertices[1].position.z +
         vertices[2].position.z) /
            3.0,
    };
    const auto edge1 = Vector3{vertices[1].position.x - vertices[0].position.x,
                               vertices[1].position.y - vertices[0].position.y,
                               vertices[1].position.z - vertices[0].position.z};
    const auto edge2 = Vector3{vertices[2].position.x - vertices[0].position.x,
                               vertices[2].position.y - vertices[0].position.y,
                               vertices[2].position.z - vertices[0].position.z};
    auto normal = cross(edge1, edge2);
    const auto normal_length = std::sqrt(
        normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (normal_length > 0.0001) {
      normal.x /= normal_length;
      normal.y /= normal_length;
      normal.z /= normal_length;
    } else {
      normal = {0.0, 0.0, 1.0};
    }
    const auto to_camera =
        Vector3{camera.x - centre.x, camera.y - centre.y, camera.z - centre.z};
    if (normal.x * to_camera.x + normal.y * to_camera.y +
            normal.z * to_camera.z <
        0.0) {
      normal.x = -normal.x;
      normal.y = -normal.y;
      normal.z = -normal.z;
    }
    auto random = [&seed]() {
      seed = seed * 1664525U + 1013904223U;
      return static_cast<double>((seed >> 8U) & 0xffffU) / 65535.0;
    };
    const auto outward_speed = 42.0 + random() * 58.0;
    const auto spin_axis_raw = Vector3{
        random() * 2.0 - 1.0, random() * 2.0 - 1.0, random() * 2.0 - 1.0};
    const auto spin_length = std::sqrt(spin_axis_raw.x * spin_axis_raw.x +
                                       spin_axis_raw.y * spin_axis_raw.y +
                                       spin_axis_raw.z * spin_axis_raw.z);
    const auto spin_axis = spin_length > 0.0001
                               ? Vector3{spin_axis_raw.x / spin_length,
                                         spin_axis_raw.y / spin_length,
                                         spin_axis_raw.z / spin_length}
                               : Vector3{0.0, 1.0, 0.0};
    GlassShardPresentationState shard;
    for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
      shard.vertices[corner] = GlassShardVertexState{
          vertices[corner].position.x, vertices[corner].position.y,
          vertices[corner].position.z, vertices[corner].u, vertices[corner].v};
    }
    shard.centre = {centre.x, centre.y, centre.z};
    shard.velocity = {
        normal.x * outward_speed + (random() * 2.0 - 1.0) * 30.0,
        normal.y * outward_speed - (28.0 + random() * 34.0),
        normal.z * outward_speed + (random() * 2.0 - 1.0) * 30.0,
    };
    shard.spin_axis = {spin_axis.x, spin_axis.y, spin_axis.z};
    shard.angular_speed = (random() * 2.0 - 1.0) * 0.34;
    shard.texture_page = material.texture_page;
    shard.clut = material.clut;
    shard.texture_bank = texture_bank;
    // Broken panes use the same authored texels but become translucent while
    // falling. This matches the retail fragment pass even for an opaque
    // intact-window material.
    shard.semi_transparent = true;
    shard.lifetime = static_cast<std::uint16_t>(24U + random() * 10.0);
    shards_.push_back(shard);
  }

  void spawn(const game::GameplaySession &gameplay,
             const RenderPresentationSnapshot &presentation,
             std::uint16_t object_index) {
    if (object_index >= presentation.objects.size()) {
      return;
    }
    const auto &object = presentation.objects[object_index];
    if (object.model >= gameplay.objectModels().size()) {
      return;
    }
    const auto *model = std::get_if<assets::GmdModel>(
        &gameplay.objectModels()[object.model].geometry);
    if (model == nullptr) {
      return;
    }
    const auto renderable_count = static_cast<std::size_t>(
        std::ranges::count_if(model->triangles(), [](const auto &triangle) {
          return triangle.flags != 0U && !triangle.degenerate;
        }));
    const auto subdivide = renderable_count <= 20U;
    auto emitted = std::size_t{};
    constexpr auto maximum_shards_per_break = std::size_t{80U};
    for (std::size_t triangle_index = 0U;
         triangle_index < model->triangles().size() &&
         emitted < maximum_shards_per_break;
         ++triangle_index) {
      const auto &triangle = model->triangles()[triangle_index];
      if (triangle.flags == 0U || triangle.degenerate) {
        continue;
      }
      std::array<SourceVertex, 3U> source{};
      for (std::size_t corner = 0U; corner < source.size(); ++corner) {
        const auto &vertex = model->vertices()[triangle.vertex_indices[corner]];
        source[corner] = SourceVertex{
            transformPoint(vertex.x, vertex.y, vertex.z, object.transform),
            static_cast<double>(triangle.uv[corner].u),
            static_cast<double>(triangle.uv[corner].v),
        };
      }
      const auto base_seed =
          0x9e3779b9U ^ (static_cast<std::uint32_t>(object_index) << 16U) ^
          static_cast<std::uint32_t>(triangle_index * 0x45d9f3bU);
      if (!subdivide) {
        appendShard(source, triangle,
                    gameplay.displayedObjectTextureBank(object_index),
                    presentation.camera, base_seed);
        ++emitted;
        continue;
      }
      const auto ab = midpoint(source[0], source[1]);
      const auto bc = midpoint(source[1], source[2]);
      const auto ca = midpoint(source[2], source[0]);
      const std::array fragments{
          std::array{source[0], ab, ca},
          std::array{ab, source[1], bc},
          std::array{ca, bc, source[2]},
          std::array{ab, bc, ca},
      };
      for (std::size_t fragment = 0U;
           fragment < fragments.size() && emitted < maximum_shards_per_break;
           ++fragment) {
        appendShard(fragments[fragment], triangle,
                    gameplay.displayedObjectTextureBank(object_index),
                    presentation.camera,
                    base_seed ^
                        static_cast<std::uint32_t>(fragment * 0x85ebca6bU));
        ++emitted;
      }
    }
  }

  std::vector<bool> destroyed_;
  std::vector<std::uint64_t> identities_;
  std::vector<GlassShardPresentationState> shards_;
};

SVECTOR
transformCylindricalBillboardVertex(const assets::GmdModel &model,
                                    const assets::GmdVertex &vertex,
                                    const assets::MissionTransform &transform,
                                    const game::CameraState &camera) {
  const auto &bounds = model.bounds();
  const auto span_x = static_cast<int>(bounds.maximum_x) - bounds.minimum_x;
  const auto span_z = static_cast<int>(bounds.maximum_z) - bounds.minimum_z;
  const auto horizontal_uses_x = span_x >= span_z;
  const auto center_x =
      (static_cast<double>(bounds.minimum_x) + bounds.maximum_x) * 0.5;
  const auto center_y =
      (static_cast<double>(bounds.minimum_y) + bounds.maximum_y) * 0.5;
  const auto center_z =
      (static_cast<double>(bounds.minimum_z) + bounds.maximum_z) * 0.5;
  const auto center = transformPoint(center_x, center_y, center_z, transform);
  const auto authored = transformPoint(vertex.x, vertex.y, vertex.z, transform);

  const auto axis = horizontal_uses_x ? std::size_t{0U} : std::size_t{2U};
  const auto axis_scale =
      std::sqrt(static_cast<double>(transform.rotation[axis]) *
                    transform.rotation[axis] +
                static_cast<double>(transform.rotation[3U + axis]) *
                    transform.rotation[3U + axis] +
                static_cast<double>(transform.rotation[6U + axis]) *
                    transform.rotation[6U + axis]) /
      4096.0;
  const auto horizontal =
      (horizontal_uses_x ? static_cast<double>(vertex.x) - center_x
                         : static_cast<double>(vertex.z) - center_z) *
      axis_scale;
  // Native aim rotates the presented target without moving the eye. Use the
  // live view axis so GLIT/YLIT rotate with that camera.
  const auto camera_right = viewBasis(camera)[0];
  return makeVertex(center.x + camera_right.x * horizontal, authored.y,
                    center.z + camera_right.z * horizontal);
}

bool reversesWinding(const assets::MissionTransform &transform) {
  const auto &matrix = transform.rotation;
  const auto determinant =
      static_cast<std::int64_t>(matrix[0]) *
          (static_cast<std::int64_t>(matrix[4]) * matrix[8] -
           static_cast<std::int64_t>(matrix[5]) * matrix[7]) -
      static_cast<std::int64_t>(matrix[1]) *
          (static_cast<std::int64_t>(matrix[3]) * matrix[8] -
           static_cast<std::int64_t>(matrix[5]) * matrix[6]) +
      static_cast<std::int64_t>(matrix[2]) *
          (static_cast<std::int64_t>(matrix[3]) * matrix[7] -
           static_cast<std::int64_t>(matrix[4]) * matrix[6]);
  return determinant < 0;
}

struct HmdPoseTransform {
  std::array<double, 9> rotation{};
  Vector3 translation{};
};

Vector3 rotateHmdPose(const HmdPoseTransform &pose, Vector3 vertex) {
  return Vector3{
      pose.rotation[0] * vertex.x + pose.rotation[1] * vertex.y +
          pose.rotation[2] * vertex.z,
      pose.rotation[3] * vertex.x + pose.rotation[4] * vertex.y +
          pose.rotation[5] * vertex.z,
      pose.rotation[6] * vertex.x + pose.rotation[7] * vertex.y +
          pose.rotation[8] * vertex.z,
  };
}

HmdPoseTransform localHmdBindPose(const assets::HmdPart &part) {
  std::array<double, 9> encoded_rotation{};
  for (std::size_t component = 0; component < encoded_rotation.size();
       ++component) {
    encoded_rotation[component] =
        static_cast<double>(part.local_transform.rotation[component]) / 4096.0;
  }
  const auto encoded_translation = Vector3{
      static_cast<double>(part.local_transform.translation[0]),
      static_cast<double>(part.local_transform.translation[1]),
      static_cast<double>(part.local_transform.translation[2]),
  };
  HmdPoseTransform result;
  // HMD stores the parent-to-part coordinate transform. The bind pose needs
  // its affine inverse: R^-1 = R^T and t^-1 = -R^T t.
  for (std::size_t row = 0; row < 3U; ++row) {
    for (std::size_t column = 0; column < 3U; ++column) {
      result.rotation[row * 3U + column] = encoded_rotation[column * 3U + row];
    }
  }
  const auto inverse_translation = rotateHmdPose(result, encoded_translation);
  result.translation = Vector3{
      -inverse_translation.x,
      -inverse_translation.y,
      -inverse_translation.z,
  };
  return result;
}

HmdPoseTransform
localHmdAnimationPose(const assets::HmdAnimationTransform &transform) {
  const auto radians = [](std::int16_t angle) {
    return static_cast<double>(angle) * 2.0 * std::numbers::pi / 4096.0;
  };
  const auto x = radians(transform.rotation[0]);
  const auto y = radians(transform.rotation[1]);
  const auto z = radians(transform.rotation[2]);
  const auto sx = std::sin(x);
  const auto cx = std::cos(x);
  const auto sy = std::sin(y);
  const auto cy = std::cos(y);
  const auto sz = std::sin(z);
  const auto cz = std::cos(z);
  // Exact floating-point equivalent of the original FUN_800d1608 matrix.
  const std::array<double, 9> encoded_rotation{
      cy * cz + sy * sx * sz,
      sy * sx * cz - cy * sz,
      sy * cx,
      sz * cx,
      cz * cx,
      -sx,
      cy * sx * sz - sy * cz,
      sy * sz + cy * sx * cz,
      cy * cx,
  };
  // PCHAN already stores a child-to-parent local pose. Its Y translation is
  // converted while decoding, unlike the parent-to-child HMD bind matrix.
  return HmdPoseTransform{
      encoded_rotation,
      Vector3{
          static_cast<double>(transform.translation[0]),
          static_cast<double>(transform.translation[1]),
          static_cast<double>(transform.translation[2]),
      },
  };
}

Vector3 applyHmdPose(const HmdPoseTransform &pose, Vector3 vertex) {
  const auto rotated = rotateHmdPose(pose, vertex);
  return Vector3{
      rotated.x + pose.translation.x,
      rotated.y + pose.translation.y,
      rotated.z + pose.translation.z,
  };
}

HmdPoseTransform composeHmdPose(const HmdPoseTransform &parent,
                                const HmdPoseTransform &local) {
  HmdPoseTransform result;
  for (std::size_t row = 0; row < 3U; ++row) {
    for (std::size_t column = 0; column < 3U; ++column) {
      auto value = 0.0;
      for (std::size_t inner = 0; inner < 3U; ++inner) {
        value += parent.rotation[row * 3U + inner] *
                 local.rotation[inner * 3U + column];
      }
      result.rotation[row * 3U + column] = value;
    }
  }
  const auto translated = applyHmdPose(parent, local.translation);
  result.translation = translated;
  return result;
}

struct ResolvedHmdPose {
  std::vector<HmdPoseTransform> parts;
  double ground_y{};
};

// HMD pose resolution runs for every visible actor on every presentation
// frame. Keep its transient tables alive across calls: at 240 Hz the former
// local vectors otherwise allocated and freed several times per actor even
// though gameplay animation advances at only 20 Hz.
struct HmdPoseScratch {
  ResolvedHmdPose pose;
  std::vector<std::uint8_t> resolved;
  std::vector<std::uint8_t> referenced;
};

const ResolvedHmdPose &resolveHmdPose(const assets::HmdModel &model,
                                      const game::ActorPose *animation_pose,
                                      HmdPoseScratch &scratch) {
  auto &result = scratch.pose;
  result.parts.resize(model.parts().size());
  scratch.resolved.assign(model.parts().size(), 0U);
  auto &resolved = scratch.resolved;
  auto resolved_count = std::size_t{};
  while (resolved_count < model.parts().size()) {
    auto made_progress = false;
    for (std::size_t index = 0; index < model.parts().size(); ++index) {
      if (resolved[index] != 0U) {
        continue;
      }
      const auto &part = model.parts()[index];
      if (part.parent >= 0 &&
          resolved[static_cast<std::size_t>(part.parent)] == 0U) {
        continue;
      }
      const auto *animated = animation_pose == nullptr
                                 ? nullptr
                                 : animation_pose->transform(index);
      const auto local = animated == nullptr ? localHmdBindPose(part)
                                             : localHmdAnimationPose(*animated);
      result.parts[index] =
          part.parent < 0
              ? local
              : composeHmdPose(
                    result.parts[static_cast<std::size_t>(part.parent)], local);
      resolved[index] = 1U;
      ++resolved_count;
      made_progress = true;
    }
    if (!made_progress) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Unresolvable HMD hierarchy"};
    }
  }

  scratch.referenced.assign(model.vertices().size(), 0U);
  auto &referenced = scratch.referenced;
  for (const auto &triangle : model.triangles()) {
    for (const auto index : triangle.vertex_indices) {
      referenced[index] = 1U;
    }
  }
  // HMD model-space Y grows downward, so the largest referenced Y is the
  // contact plane. Preserve the axis direction and only move it to zero.
  result.ground_y = std::numeric_limits<double>::lowest();
  for (std::size_t index = 0; index < model.vertices().size(); ++index) {
    const auto &vertex = model.vertices()[index];
    const auto posed = applyHmdPose(result.parts[model.vertexParts()[index]],
                                    Vector3{
                                        static_cast<double>(vertex.x),
                                        static_cast<double>(vertex.y),
                                        static_cast<double>(vertex.z),
                                    });
    if (referenced[index] != 0U) {
      result.ground_y = std::max(result.ground_y, posed.y);
    }
  }
  if (result.ground_y == std::numeric_limits<double>::lowest()) {
    result.ground_y = 0.0;
  }
  return result;
}

bool hasLegacyHmdBones(const assets::HmdModel &model,
                       const game::SceneObject &object) noexcept {
  return !model.parts().empty() &&
         model.parts().size() <= object.legacy_hmd_bones.size() &&
         object.legacy_hmd_bone_count >= model.parts().size();
}

std::uint64_t npcAnimationPhase(const game::SceneObject &object,
                                std::uint16_t object_index) noexcept {
  return object.legacy_hmd_root_space || object.legacy_hmd_bone_count != 0U
             ? 0U
             : static_cast<std::uint64_t>(object_index) * 7U;
}

std::optional<assets::MissionTransform> posedActorPartTransform(
    const assets::HmdModel &model, const game::SceneObject &object,
    const game::ActorPose &animation_pose, HmdPoseScratch &pose_scratch,
    std::string_view part_prefix,
    const ResolvedHmdPose *prepared_pose = nullptr) {
  const auto part = std::ranges::find_if(
      model.parts(), [part_prefix](const assets::HmdPart &candidate) {
        return candidate.name.starts_with(part_prefix);
      });
  if (part == model.parts().end()) {
    return std::nullopt;
  }
  const auto part_index =
      static_cast<std::size_t>(std::distance(model.parts().begin(), part));
  if (hasLegacyHmdBones(model, object)) {
    return object.legacy_hmd_bones[part_index];
  }
  const auto &pose = prepared_pose != nullptr
                         ? *prepared_pose
                         : resolveHmdPose(model, &animation_pose, pose_scratch);
  const auto &local = pose.parts[part_index];
  std::array<double, 9> actor_rotation{};
  for (std::size_t index = 0; index < actor_rotation.size(); ++index) {
    actor_rotation[index] =
        static_cast<double>(object.transform.rotation[index]) / 4096.0;
  }
  std::array<std::int16_t, 9> combined_rotation{};
  for (std::size_t row = 0; row < 3U; ++row) {
    for (std::size_t column = 0; column < 3U; ++column) {
      auto value = 0.0;
      for (std::size_t inner = 0; inner < 3U; ++inner) {
        value += actor_rotation[row * 3U + inner] *
                 local.rotation[inner * 3U + column];
      }
      combined_rotation[row * 3U + column] = static_cast<std::int16_t>(
          std::clamp(std::lround(value * 4096.0), -32768L, 32767L));
    }
  }
  const auto world =
      transformPoint(local.translation.x,
                     local.translation.y -
                         (object.legacy_hmd_root_space ? 0.0 : pose.ground_y),
                     local.translation.z, object.transform);
  return assets::MissionTransform{
      combined_rotation,
      static_cast<std::int32_t>(std::lround(world.x)),
      static_cast<std::int32_t>(std::lround(-world.y)),
      static_cast<std::int32_t>(std::lround(world.z)),
  };
}

std::optional<assets::MissionTransform> posedActorPartTransform(
    const assets::HmdModel &model, const game::SceneObject &object,
    const game::ActorPose &animation_pose, std::string_view part_prefix) {
  // Effect and muzzle paths do not own the frame renderer's scratch object.
  // Keep one fallback per render thread instead of reintroducing three heap
  // allocations for every attachment lookup.
  static thread_local HmdPoseScratch pose_scratch;
  return posedActorPartTransform(model, object, animation_pose, pose_scratch,
                                 part_prefix);
}

SVECTOR transformHmdVertex(const Vector3 &vertex,
                           const assets::MissionTransform &transform) {
  const auto point = transformPoint(vertex.x, vertex.y, vertex.z, transform);
  return makeVertex(point.x, point.y, point.z);
}

std::optional<Vector3> posedActorPartAnchor(const assets::HmdModel &model,
                                            const game::SceneObject &object,
                                            const game::ActorPose &pose,
                                            HmdPoseScratch &pose_scratch,
                                            std::string_view part_prefix) {
  const auto part = std::ranges::find_if(
      model.parts(), [part_prefix](const assets::HmdPart &candidate) {
        return candidate.name.starts_with(part_prefix);
      });
  if (part == model.parts().end()) {
    return std::nullopt;
  }
  const auto part_index =
      static_cast<std::uint16_t>(std::distance(model.parts().begin(), part));
  const auto direct_legacy_pose = hasLegacyHmdBones(model, object);
  const auto *resolved_pose = direct_legacy_pose
                                  ? nullptr
                                  : &resolveHmdPose(model, &pose, pose_scratch);
  auto minimum = Vector3{
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
  };
  auto maximum = Vector3{
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
  };
  auto found = false;
  for (std::size_t index = 0; index < model.vertices().size(); ++index) {
    if (model.vertexParts()[index] != part_index) {
      continue;
    }
    const auto &source = model.vertices()[index];
    auto vertex =
        direct_legacy_pose
            ? transformPoint(static_cast<double>(source.x),
                             static_cast<double>(source.y),
                             static_cast<double>(source.z),
                             object.legacy_hmd_bones[part_index])
            : applyHmdPose(resolved_pose->parts[model.vertexParts()[index]],
                           Vector3{static_cast<double>(source.x),
                                   static_cast<double>(source.y),
                                   static_cast<double>(source.z)});
    if (!direct_legacy_pose && !object.legacy_hmd_root_space) {
      vertex.y -= resolved_pose->ground_y;
    }
    minimum.x = std::min(minimum.x, vertex.x);
    minimum.y = std::min(minimum.y, vertex.y);
    minimum.z = std::min(minimum.z, vertex.z);
    maximum.x = std::max(maximum.x, vertex.x);
    maximum.y = std::max(maximum.y, vertex.y);
    maximum.z = std::max(maximum.z, vertex.z);
    found = true;
  }
  if (!found) {
    return std::nullopt;
  }
  const auto centre = Vector3{
      (minimum.x + maximum.x) * 0.5,
      (minimum.y + maximum.y) * 0.5,
      (minimum.z + maximum.z) * 0.5,
  };
  return direct_legacy_pose
             ? centre
             : transformPoint(centre.x, centre.y, centre.z, object.transform);
}

std::optional<Vector3> sceneObjectCalloutAnchor(
    const game::GameplaySession &gameplay,
    const game::ActorAnimationBank &actor_animations, std::uint64_t actor_tick,
    std::span<const game::SceneObject> presentation_objects,
    std::uint16_t scene_object, bool headshot, HmdPoseScratch &pose_scratch) {
  if (scene_object >= presentation_objects.size()) {
    return std::nullopt;
  }
  const auto &object = presentation_objects[scene_object];
  if (object.model >= gameplay.objectModels().size()) {
    return std::nullopt;
  }
  const auto &object_model = gameplay.objectModels()[object.model];
  const auto &geometry = object_model.geometry;
  const auto *model = std::get_if<assets::HmdModel>(&geometry);
  if (model != nullptr) {
    const auto *state = gameplay.npcState(scene_object);
    const auto pose =
        state == nullptr
            ? actor_animations.enemyPose(
                  actor_tick, static_cast<std::uint64_t>(scene_object) * 7U)
            : actor_animations.npcPose(gameplay.npcAnimation(scene_object),
                                       state->locomotion_animation_tick,
                                       state->animation_tick,
                                       npcAnimationPhase(object, scene_object));
    if (const auto anchor = posedActorPartAnchor(
            *model, object, pose, pose_scratch,
            headshot ? std::string_view{"Head"} : std::string_view{"Chest"})) {
      return anchor;
    }
  }
  if (object_model.bounds) {
    const auto &bounds = *object_model.bounds;
    return transformPoint(
        (static_cast<double>(bounds.minimum_x) + bounds.maximum_x) * 0.5,
        (static_cast<double>(bounds.minimum_y) + bounds.maximum_y) * 0.5,
        (static_cast<double>(bounds.minimum_z) + bounds.maximum_z) * 0.5,
        object.transform);
  }
  return transformPoint(0.0, 0.0, 0.0, object.transform);
}

std::optional<Vector3>
aimTargetAnchor(const game::GameplaySession &gameplay,
                const game::ActorAnimationBank &actor_animations,
                std::uint64_t actor_tick,
                std::span<const game::SceneObject> presentation_objects,
                HmdPoseScratch &pose_scratch) {
  const auto target = gameplay.aimTarget();
  return target ? sceneObjectCalloutAnchor(gameplay, actor_animations,
                                           actor_tick, presentation_objects,
                                           *target, gameplay.headshotTargeted(),
                                           pose_scratch)
                : std::nullopt;
}

void worldCalloutAnchors(
    const game::GameplaySession &gameplay,
    const game::ActorAnimationBank &actor_animations, std::uint64_t actor_tick,
    std::span<const game::SceneObject> presentation_objects,
    HmdPoseScratch &pose_scratch, std::vector<WorldCalloutAnchor> &result) {
  result.clear();
  if (result.capacity() < gameplay.legacyWorldCallouts().size()) {
    result.reserve(gameplay.legacyWorldCallouts().size());
  }
  for (const auto &callout : gameplay.legacyWorldCallouts()) {
    const auto anchor = sceneObjectCalloutAnchor(
        gameplay, actor_animations, actor_tick, presentation_objects,
        callout.object, callout.headshot, pose_scratch);
    if (!anchor || callout.text.empty()) {
      continue;
    }
    result.push_back(WorldCalloutAnchor{*anchor, callout.object, callout.text,
                                        callout.headshot});
  }
}

enum class OpticSubjectStyle : std::uint8_t;

[[nodiscard]] constexpr VertexColor
opticSubjectColor(OpticSubjectStyle style) noexcept;

void submitStats(RenderStats &stats, int depth,
                 std::span<const long> projected);

void renderProjectedOpticFrame(const OpticProjectedFrame &frame,
                               OpticSubjectStyle style, std::size_t echo_index,
                               std::vector<OT_TAG> &ordering_table,
                               PrimitiveBuffer &primitives,
                               RenderStats &stats) {
  const auto color = opticSubjectColor(style);
  for (const auto &triangle : frame) {
    auto &primitive = primitives.optic_trails.emplace_back();
    // SVDN2 replays the cached 0x18-byte packet as an opaque POLY_F3
    // (opcode 0x20). The three history colors are flat packet colors; no
    // Gouraud interpolation or ABR/semitransparency participates.
    setPolyF3(&primitive);
    setRGB0(&primitive, color.red, color.green, color.blue);
    setProjected(&primitive.x0, triangle.packed_xy[0]);
    setProjected(&primitive.x1, triangle.packed_xy[1]);
    setProjected(&primitive.x2, triangle.packed_xy[2]);

    const auto precise_begin = PGXP_GetIndex(0);
    const auto precise_emitted =
        triangle.precise_valid[0] && triangle.precise_valid[1] &&
        triangle.precise_valid[2] && precise_begin != 0xffffU &&
        static_cast<unsigned int>(precise_begin) + 3U < 0xffffU;
    if (precise_emitted) {
      // PGXP is renderer-thread-owned. Once the atomic three-slot preflight
      // succeeds these writes cannot fail, and addPrim consumes their end
      // cursor as one precise triangle.
      for (auto corner = std::size_t{}; corner < 3U; ++corner) {
        auto precise = triangle.precise[corner];
        static_cast<void>(PGXP_EmitCacheData(&precise));
      }
    }
    if (!precise_emitted) {
      // Clear any stale transform latch without emitting a partial replay.
      static_cast<void>(PGXP_GetIndex(1));
    }
    const auto depth =
        sf::platform::retailOpticEchoDepth(triangle.ordering_depth, echo_index);
    addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
            &primitive);
    submitStats(stats, depth, triangle.packed_xy);
  }
}

enum class OpticSubjectStyle : std::uint8_t {
  nightvision,
  virus_scanner,
  nightvision_trail_oldest,
  nightvision_trail_middle,
  nightvision_trail_recent,
};

[[nodiscard]] constexpr bool
isNightvisionTrail(OpticSubjectStyle style) noexcept {
  return style == OpticSubjectStyle::nightvision_trail_oldest ||
         style == OpticSubjectStyle::nightvision_trail_middle ||
         style == OpticSubjectStyle::nightvision_trail_recent;
}

[[nodiscard]] constexpr VertexColor
opticSubjectColor(OpticSubjectStyle style) noexcept {
  switch (style) {
  case OpticSubjectStyle::nightvision:
    // Retail live SVD silhouette (opaque POLY_F3 word 0x2000ff48).
    return {72U, 255U, 0U};
  case OpticSubjectStyle::virus_scanner:
    // Virus-scanner actors use the same opaque silhouette path in orange.
    return {249U, 135U, 0U};
  case OpticSubjectStyle::nightvision_trail_oldest:
    return {5U, 26U, 98U};
  case OpticSubjectStyle::nightvision_trail_middle:
    return {7U, 104U, 149U};
  case OpticSubjectStyle::nightvision_trail_recent:
    return {0U, 165U, 171U};
  }
  return {};
}

void captureOpticTriangle(OpticProjectedFrame &capture, long xy0, long xy1,
                          long xy2, int depth, ushort cache_end) {
  auto triangle = OpticProjectedTriangle{.packed_xy = {xy0, xy1, xy2},
                                         .ordering_depth = depth};
  if (cache_end != 0xffffU && cache_end >= 3U) {
    for (auto corner = std::size_t{}; corner < 3U; ++corner) {
      triangle.precise_valid[corner] =
          PGXP_GetCacheDataExact(
              &triangle.precise[corner],
              static_cast<ushort>(cache_end - 3U + corner)) != 0;
    }
  }
  capture.push_back(std::move(triangle));
}

void renderOpticHmdObject(
    const assets::HmdModel &model, std::span<const Vector3> world_vertices,
    OpticSubjectStyle style, const MATRIX &view,
    std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats, std::size_t triangle_stride = 1U, int ordering_bias = 0,
    OpticProjectedFrame *capture = nullptr, bool emit_primitive = true) {
  if (world_vertices.size() != model.vertices().size() ||
      triangle_stride == 0U) {
    return;
  }
  for (auto triangle_index = std::size_t{};
       triangle_index < model.triangles().size();
       triangle_index += triangle_stride) {
    const auto &triangle = model.triangles()[triangle_index];
    const auto &first = world_vertices[triangle.vertex_indices[0]];
    const auto &second = world_vertices[triangle.vertex_indices[1]];
    const auto &third = world_vertices[triangle.vertex_indices[2]];
    auto v0 = makeVertex(first.x, first.y, first.z);
    auto v1 = makeVertex(second.x, second.y, second.z);
    auto v2 = makeVertex(third.x, third.y, third.z);
    const std::array positions{v0, v1, v2};
    if (classifyNearPlane(view, positions) != NearPlaneStatus::inside) {
      ++stats.rejected;
      continue;
    }
    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    const auto capture_cache_end = capture != nullptr
                                       ? PGXP_GetIndex(emit_primitive ? 0 : 1)
                                       : static_cast<ushort>(0xffffU);
    if (depth <= 0 || !frontFacing(xy0, xy1, xy2, 3U) ||
        triviallyOutsideViewport(std::array<long, 3U>{xy0, xy1, xy2})) {
      if (emit_primitive) {
        ++stats.rejected;
      }
      continue;
    }
    if (capture != nullptr) {
      captureOpticTriangle(*capture, xy0, xy1, xy2, depth, capture_cache_end);
    }
    if (!emit_primitive) {
      continue;
    }
    const auto color = opticSubjectColor(style);
    auto &primitive = isNightvisionTrail(style)
                          ? primitives.optic_trails.emplace_back()
                          : primitives.optic_subjects.emplace_back();
    setPolyF3(&primitive);
    setRGB0(&primitive, color.red, color.green, color.blue);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    addPrim(&ordering_table[std::clamp(depth + ordering_bias, 1,
                                       ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

void renderOpticGmdObject(const assets::GmdModel &model,
                          const assets::MissionTransform &transform,
                          OpticSubjectStyle style, const MATRIX &view,
                          std::vector<OT_TAG> &ordering_table,
                          PrimitiveBuffer &primitives, RenderStats &stats,
                          OpticProjectedFrame *capture = nullptr,
                          bool emit_primitive = true) {
  if (model.vertices().empty()) {
    return;
  }
  const auto reverse_winding = reversesWinding(transform);
  for (const auto &triangle : model.triangles()) {
    if (triangle.flags == 0U || triangle.degenerate) {
      continue;
    }
    const auto first = transformPoint(
        model.vertices()[triangle.vertex_indices[0]].x,
        model.vertices()[triangle.vertex_indices[0]].y,
        model.vertices()[triangle.vertex_indices[0]].z, transform);
    const auto second = transformPoint(
        model.vertices()[triangle.vertex_indices[1]].x,
        model.vertices()[triangle.vertex_indices[1]].y,
        model.vertices()[triangle.vertex_indices[1]].z, transform);
    const auto third = transformPoint(
        model.vertices()[triangle.vertex_indices[2]].x,
        model.vertices()[triangle.vertex_indices[2]].y,
        model.vertices()[triangle.vertex_indices[2]].z, transform);
    auto v0 = makeVertex(first.x, first.y, first.z);
    auto v1 = makeVertex(second.x, second.y, second.z);
    auto v2 = makeVertex(third.x, third.y, third.z);
    const std::array positions{v0, v1, v2};
    if (classifyNearPlane(view, positions) != NearPlaneStatus::inside) {
      ++stats.rejected;
      continue;
    }
    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    const auto capture_cache_end = capture != nullptr
                                       ? PGXP_GetIndex(emit_primitive ? 0 : 1)
                                       : static_cast<ushort>(0xffffU);
    const auto visible_face = frontFacing(xy0, xy1, xy2, 3U) != reverse_winding;
    if (depth <= 0 || !visible_face ||
        triviallyOutsideViewport(std::array<long, 3U>{xy0, xy1, xy2})) {
      if (emit_primitive) {
        ++stats.rejected;
      }
      continue;
    }
    if (capture != nullptr) {
      captureOpticTriangle(*capture, xy0, xy1, xy2, depth, capture_cache_end);
    }
    if (!emit_primitive) {
      continue;
    }
    const auto color = opticSubjectColor(style);
    auto &primitive = primitives.optic_subjects.emplace_back();
    setPolyF3(&primitive);
    setRGB0(&primitive, color.red, color.green, color.blue);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

std::optional<Vector3> weaponMuzzleAnchor(const game::GameplaySession &gameplay,
                                          const assets::HmdModel &actor_model,
                                          const game::SceneObject &actor_object,
                                          const game::ActorPose &actor_pose,
                                          game::WeaponId weapon) {
  const auto hand_transform = posedActorPartTransform(actor_model, actor_object,
                                                      actor_pose, "RightHan");
  if (!hand_transform) {
    return std::nullopt;
  }
  const auto *weapon_model = gameplay.weaponModel(weapon);
  if (weapon_model == nullptr || !weapon_model->bounds) {
    return transformPoint(0.0, 64.0, 0.0, *hand_transform);
  }
  const auto &bounds = *weapon_model->bounds;
  // Retail weapon GMDs are authored with the barrel running along local +Y
  // (for example GLOCK17 reaches Y=23 while M16 reaches Y=84).  Using +Z
  // placed every attached tracer/light beside the hand instead of at the
  // rendered barrel tip.
  return transformPoint(
      (static_cast<double>(bounds.minimum_x) + bounds.maximum_x) * 0.5,
      static_cast<double>(bounds.maximum_y),
      (static_cast<double>(bounds.minimum_z) + bounds.maximum_z) * 0.5,
      *hand_transform);
}

assets::MissionTransform playerTransform(const game::PlayerState &player,
                                         std::int32_t model_heading);

game::SceneObject
presentedPlayerObject(const RenderPresentationSnapshot &presentation) {
  if (presentation.legacy_player) {
    return *presentation.legacy_player;
  }
  return game::SceneObject{
      .transform = playerTransform(presentation.player,
                                   presentation.player_model_heading),
  };
}

game::SceneObject currentPlayerObject(const game::GameplaySession &gameplay) {
  if (const auto *legacy_player = gameplay.legacyPlayerPresentation()) {
    return *legacy_player;
  }
  return game::SceneObject{
      .transform =
          playerTransform(gameplay.player(), gameplay.playerModelHeading()),
  };
}

std::optional<Vector3>
playerWeaponMuzzleAnchor(const game::GameplaySession &gameplay,
                         const game::ActorAnimationBank &actor_animations,
                         const game::SceneObject &player_object,
                         game::WeaponId weapon) {
  const auto *model =
      std::get_if<assets::HmdModel>(&gameplay.playerModel().geometry);
  if (model == nullptr) {
    return std::nullopt;
  }
  const auto action_tick =
      gameplay.playerAction() == game::PlayerActionState::ready
          ? gameplay.playerAnimationTick()
          : gameplay.playerActionAnimationTick();
  const auto pose = actor_animations.playerPose(
      gameplay.playerAnimation(), gameplay.playerAnimationTick(), action_tick);
  return weaponMuzzleAnchor(gameplay, *model, player_object, pose, weapon);
}

std::optional<Vector3>
npcWeaponMuzzleAnchor(const game::GameplaySession &gameplay,
                      const game::ActorAnimationBank &actor_animations,
                      const game::SceneObject &object,
                      std::uint16_t scene_object, game::WeaponId weapon) {
  if (object.model >= gameplay.objectModels().size()) {
    return std::nullopt;
  }
  const auto *state = gameplay.npcState(scene_object);
  const auto *model = std::get_if<assets::HmdModel>(
      &gameplay.objectModels()[object.model].geometry);
  if (state == nullptr || model == nullptr) {
    return std::nullopt;
  }
  const auto pose = actor_animations.npcPose(
      gameplay.npcAnimation(scene_object), state->locomotion_animation_tick,
      state->animation_tick, npcAnimationPhase(object, scene_object));
  return weaponMuzzleAnchor(gameplay, *model, object, pose, weapon);
}

std::optional<Vector3>
weaponMuzzleAnchorForGuestSlot(const game::GameplaySession &gameplay,
                               const game::ActorAnimationBank &actor_animations,
                               const RenderPresentationSnapshot *presentation,
                               std::int16_t guest_slot,
                               game::WeaponId player_weapon) {
  if (presentation != nullptr && guest_slot >= 0 &&
      guest_slot == presentation->player_guest_slot) {
    return playerWeaponMuzzleAnchor(gameplay, actor_animations,
                                    presentedPlayerObject(*presentation),
                                    player_weapon);
  }
  const auto guest_slots = gameplay.legacyGuestSlotsBySceneObject();
  const auto mapped = std::ranges::find(guest_slots, guest_slot);
  if (mapped != guest_slots.end()) {
    const auto scene =
        static_cast<std::size_t>(std::distance(guest_slots.begin(), mapped));
    const auto objects =
        presentation != nullptr
            ? std::span<const game::SceneObject>{presentation->objects}
            : gameplay.objects();
    if (scene >= objects.size()) {
      return std::nullopt;
    }
    if (objects[scene].class_id == 0U) {
      const auto player_object = presentation != nullptr
                                     ? presentedPlayerObject(*presentation)
                                     : currentPlayerObject(gameplay);
      return playerWeaponMuzzleAnchor(gameplay, actor_animations, player_object,
                                      player_weapon);
    }
    const auto *state = gameplay.npcState(static_cast<std::uint16_t>(scene));
    if (state == nullptr) {
      return std::nullopt;
    }
    return npcWeaponMuzzleAnchor(gameplay, actor_animations, objects[scene],
                                 static_cast<std::uint16_t>(scene),
                                 state->weapon);
  }
  // Controller +0x20 is optional provenance and is initialized to -1 by the
  // retail allocator.  A stale/non-resident guest slot is not implicitly the
  // player: doing so drags exact NPC tracers by Gabe's interpolation delta.
  return std::nullopt;
}

std::optional<Vector3>
actorRootForGuestSlot(const game::GameplaySession &gameplay,
                      const RenderPresentationSnapshot *presentation,
                      std::int16_t guest_slot) {
  const auto guest_slots = gameplay.legacyGuestSlotsBySceneObject();
  const auto mapped = std::ranges::find(guest_slots, guest_slot);
  if (mapped == guest_slots.end()) {
    return std::nullopt;
  }
  const auto scene =
      static_cast<std::size_t>(std::distance(guest_slots.begin(), mapped));
  const auto objects =
      presentation != nullptr
          ? std::span<const game::SceneObject>{presentation->objects}
          : gameplay.objects();
  if (scene >= objects.size()) {
    return std::nullopt;
  }
  const auto &mapped_object = objects[scene];
  const auto object =
      mapped_object.class_id == 0U
          ? (presentation != nullptr ? presentedPlayerObject(*presentation)
                                     : currentPlayerObject(gameplay))
          : mapped_object;
  return transformPoint(0.0, 0.0, 0.0, object.transform);
}

Vector3 attachedEffectCentre(const game::GameplaySession &gameplay,
                             const game::ActorAnimationBank &actor_animations,
                             const RenderPresentationSnapshot &presentation,
                             const game::GameplayEffect &effect) {
  const auto fallback = Vector3{effect.x, effect.y, effect.z};
  if (effect.attachment == game::GameplayEffectAttachment::world) {
    return fallback;
  }
  if (effect.attachment == game::GameplayEffectAttachment::player_muzzle) {
    return playerWeaponMuzzleAnchor(gameplay, actor_animations,
                                    presentedPlayerObject(presentation),
                                    gameplay.hud().inventory().current())
        .value_or(fallback);
  }
  if (effect.attachment == game::GameplayEffectAttachment::player_body) {
    const auto &player = presentation.player;
    return Vector3{
        player.x + effect.attachment_offset_x,
        player.y + effect.attachment_offset_y,
        player.z + effect.attachment_offset_z,
    };
  }
  if (effect.owner_object >= presentation.objects.size()) {
    return fallback;
  }
  const auto *state = gameplay.npcState(effect.owner_object);
  const auto &object = presentation.objects[effect.owner_object];
  const auto *model = std::get_if<assets::HmdModel>(
      &gameplay.objectModels()[object.model].geometry);
  if (state == nullptr || model == nullptr) {
    return fallback;
  }
  if (effect.attachment == game::GameplayEffectAttachment::npc_body) {
    const auto root = transformPoint(0.0, 0.0, 0.0, object.transform);
    return Vector3{
        root.x + effect.attachment_offset_x,
        root.y + effect.attachment_offset_y,
        root.z + effect.attachment_offset_z,
    };
  }
  return npcWeaponMuzzleAnchor(gameplay, actor_animations, object,
                               effect.owner_object, state->weapon)
      .value_or(fallback);
}

void configurePrimitive(POLY_GT3 &primitive,
                        const std::array<TexturedVertex, 3> &vertices,
                        TexturedMaterial material) {
  setPolyGT3(&primitive);
  setRGB0(&primitive, quantizeColorOrUv(vertices[0].red),
          quantizeColorOrUv(vertices[0].green),
          quantizeColorOrUv(vertices[0].blue));
  setRGB1(&primitive, quantizeColorOrUv(vertices[1].red),
          quantizeColorOrUv(vertices[1].green),
          quantizeColorOrUv(vertices[1].blue));
  setRGB2(&primitive, quantizeColorOrUv(vertices[2].red),
          quantizeColorOrUv(vertices[2].green),
          quantizeColorOrUv(vertices[2].blue));
  setUV3(&primitive, quantizeColorOrUv(vertices[0].u),
         quantizeColorOrUv(vertices[0].v), quantizeColorOrUv(vertices[1].u),
         quantizeColorOrUv(vertices[1].v), quantizeColorOrUv(vertices[2].u),
         quantizeColorOrUv(vertices[2].v));
  primitive.clut =
      material.resident
          ? material.clut
          : relocateClut(material.clut, material.texture_page,
                         material.texture_bank, material.texture_source_page);
  primitive.tpage =
      material.resident
          ? material.texture_page
          : relocateTexturePage(material.texture_page, material.texture_bank,
                                material.texture_source_page);
  if (material.semi_transparent) {
    setSemiTrans(&primitive, 1);
  }
}

void configurePrimitive(POLY_GT4 &primitive,
                        const std::array<TexturedVertex, 4> &vertices,
                        TexturedMaterial material) {
  setPolyGT4(&primitive);
  setRGB0(&primitive, quantizeColorOrUv(vertices[0].red),
          quantizeColorOrUv(vertices[0].green),
          quantizeColorOrUv(vertices[0].blue));
  setRGB1(&primitive, quantizeColorOrUv(vertices[1].red),
          quantizeColorOrUv(vertices[1].green),
          quantizeColorOrUv(vertices[1].blue));
  setRGB2(&primitive, quantizeColorOrUv(vertices[2].red),
          quantizeColorOrUv(vertices[2].green),
          quantizeColorOrUv(vertices[2].blue));
  setRGB3(&primitive, quantizeColorOrUv(vertices[3].red),
          quantizeColorOrUv(vertices[3].green),
          quantizeColorOrUv(vertices[3].blue));
  setUV4(&primitive, quantizeColorOrUv(vertices[0].u),
         quantizeColorOrUv(vertices[0].v), quantizeColorOrUv(vertices[1].u),
         quantizeColorOrUv(vertices[1].v), quantizeColorOrUv(vertices[2].u),
         quantizeColorOrUv(vertices[2].v), quantizeColorOrUv(vertices[3].u),
         quantizeColorOrUv(vertices[3].v));
  primitive.clut =
      material.resident
          ? material.clut
          : relocateClut(material.clut, material.texture_page,
                         material.texture_bank, material.texture_source_page);
  primitive.tpage =
      material.resident
          ? material.texture_page
          : relocateTexturePage(material.texture_page, material.texture_bank,
                                material.texture_source_page);
  if (material.semi_transparent) {
    setSemiTrans(&primitive, 1);
  }
}

TexturedVertex
emdTexturedVertex(const SVECTOR &position, const assets::EmdSection &section,
                  const assets::EmdPolygon &polygon, std::size_t corner,
                  std::span<const std::uint16_t> live_colors = {},
                  bool apply_lighting = true,
                  game::DynamicLightPoint normal = {},
                  const game::DynamicLightFrame *dynamic_lights = nullptr) {
  const auto vertex = polygon.vertex_indices[corner];
  return makeTexturedVertex(position, polygon.uv[corner],
                            decodeColor(live_colors.empty()
                                            ? section.vertices[vertex].color
                                            : live_colors[vertex]),
                            apply_lighting, normal, dynamic_lights);
}

using OrderingDepthTransform = int (*)(int depth);

int worldOrderingDepth(int depth) { return depth; }

int fireOrderingDepth(int depth) {
  // The original GsSortSprite path uses 3/4 of RotTransPers depth and pulls
  // CFIRE another 32 OT slots toward the camera. This keeps the centered
  // billboard above coplanar street geometry without moving it in 3D.
  return depth - (depth >> 2) - 32;
}

void submitClippedTriangle(
    std::span<const TexturedVertex> vertices, TexturedMaterial material,
    bool two_sided, bool reverse_winding, const MATRIX &view,
    std::vector<OT_TAG> &ordering_table,
    StableFrameVector<POLY_GT3> &primitive_buffer, RenderStats &stats,
    OrderingDepthTransform ordering_depth = worldOrderingDepth,
    bool terrain_depth_cue = false, bool depth_cue_enabled = true,
    long depth_cue_floor = 0L) {
  if (vertices.size() != 3U) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Near-plane submission expects one triangle"};
  }
  std::array<float, 4> source_uv_bounds{
      static_cast<float>(vertices[0].u),
      static_cast<float>(vertices[0].v),
      static_cast<float>(vertices[0].u),
      static_cast<float>(vertices[0].v),
  };
  for (const auto &vertex : vertices.subspan(1U)) {
    source_uv_bounds[0] =
        std::min(source_uv_bounds[0], static_cast<float>(vertex.u));
    source_uv_bounds[1] =
        std::min(source_uv_bounds[1], static_cast<float>(vertex.v));
    source_uv_bounds[2] =
        std::max(source_uv_bounds[2], static_cast<float>(vertex.u));
    source_uv_bounds[3] =
        std::max(source_uv_bounds[3], static_cast<float>(vertex.v));
  }
  const auto clipped = core::clipConvexPolygon<TexturedVertex, 4>(
      vertices,
      [&view](const TexturedVertex &vertex) {
        return cameraDepth(view, vertex) - active_near_clip_depth;
      },
      interpolate);
  if (clipped.count < 3U) {
    ++stats.rejected;
    return;
  }
  for (std::size_t index = 1; index + 1U < clipped.count; ++index) {
    const std::array triangle{
        clipped.vertices[0],
        clipped.vertices[index],
        clipped.vertices[index + 1U],
    };
    auto v0 = makeVertex(triangle[0].x, triangle[0].y, triangle[0].z);
    auto v1 = makeVertex(triangle[1].x, triangle[1].y, triangle[1].z);
    auto v2 = makeVertex(triangle[2].x, triangle[2].y, triangle[2].z);
    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    const std::array precise_uv{
        static_cast<float>(triangle[0].u), static_cast<float>(triangle[0].v),
        static_cast<float>(triangle[1].u), static_cast<float>(triangle[1].v),
        static_cast<float>(triangle[2].u), static_cast<float>(triangle[2].v),
    };
    PGXP_SetLastTextureCoords(precise_uv.data(), 3, source_uv_bounds.data());
    const auto visible_face = frontFacing(xy0, xy1, xy2, 3U) != reverse_winding;
    if (depth <= 0 || (!two_sided && !visible_face)) {
      ++stats.rejected;
      continue;
    }

    auto &primitive = primitive_buffer.emplace_back();
    configurePrimitive(primitive, triangle, material);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    if (depth_cue_enabled && !material.semi_transparent) {
      const auto cue = [terrain_depth_cue, depth_cue_floor](double camera_z) {
        return std::max(depth_cue_floor, terrain_depth_cue
                                             ? retailTerrainDepthCue(camera_z)
                                             : retailObjectDepthCue(camera_z));
      };
      depthCuePrimitive(primitive, {cue(cameraDepth(view, triangle[0])),
                                    cue(cameraDepth(view, triangle[1])),
                                    cue(cameraDepth(view, triangle[2]))});
    }
    addPrim(&ordering_table[std::clamp(ordering_depth(depth), 1,
                                       ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

void renderGlassShards(std::span<const GlassShardPresentationState> shards,
                       double interpolation_amount, const MATRIX &view,
                       std::vector<OT_TAG> &ordering_table,
                       PrimitiveBuffer &primitives, RenderStats &stats) {
  interpolation_amount = std::clamp(interpolation_amount, 0.0, 1.0);
  constexpr auto gravity_per_update = 11.0;
  for (const auto &shard : shards) {
    const auto time = static_cast<double>(shard.age) + interpolation_amount;
    const auto angle = shard.angular_speed * time;
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    const auto axis =
        Vector3{shard.spin_axis.x, shard.spin_axis.y, shard.spin_axis.z};
    const auto centre = Vector3{shard.centre.x, shard.centre.y, shard.centre.z};
    const auto translation = Vector3{
        shard.velocity.x * time,
        shard.velocity.y * time + gravity_per_update * time * time * 0.5,
        shard.velocity.z * time,
    };
    std::array<TexturedVertex, 3U> vertices{};
    for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
      const auto &source = shard.vertices[corner];
      const auto local = Vector3{source.x - centre.x, source.y - centre.y,
                                 source.z - centre.z};
      const auto axis_cross = cross(axis, local);
      const auto axis_dot =
          axis.x * local.x + axis.y * local.y + axis.z * local.z;
      const auto one_minus_cosine = 1.0 - cosine;
      const auto rotated = Vector3{
          local.x * cosine + axis_cross.x * sine +
              axis.x * axis_dot * one_minus_cosine,
          local.y * cosine + axis_cross.y * sine +
              axis.y * axis_dot * one_minus_cosine,
          local.z * cosine + axis_cross.z * sine +
              axis.z * axis_dot * one_minus_cosine,
      };
      const auto position = Vector3{centre.x + rotated.x + translation.x,
                                    centre.y + rotated.y + translation.y,
                                    centre.z + rotated.z + translation.z};
      auto lit = makeTexturedVertex(
          makeVertex(position.x, position.y, position.z),
          assets::EmdUv{static_cast<std::uint8_t>(
                            std::clamp(std::lround(source.u), 0L, 255L)),
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(source.v), 0L, 255L))},
          VertexColor{128U, 128U, 128U});
      lit.x = position.x;
      lit.y = position.y;
      lit.z = position.z;
      lit.u = source.u;
      lit.v = source.v;
      vertices[corner] = lit;
    }
    submitClippedTriangle(
        vertices,
        TexturedMaterial{shard.texture_page, shard.clut, shard.semi_transparent,
                         false, shard.texture_bank},
        true, false, view, ordering_table, primitives.objects, stats);
  }
}

void renderGmdObject(const assets::GmdModel &model,
                     const game::SceneObject &object,
                     game::ObjectVisualEffect visual_effect,
                     unsigned int texture_bank, const game::CameraState &camera,
                     const MATRIX &view, std::vector<OT_TAG> &ordering_table,
                     PrimitiveBuffer &primitives, RenderStats &stats,
                     VertexColor back_color = {128U, 128U, 128U},
                     bool force_two_sided = false,
                     bool force_semi_transparent = false) {
  const auto reverse_winding = reversesWinding(object.transform);
  for (auto triangle_index = std::size_t{};
       triangle_index < model.triangles().size(); ++triangle_index) {
    const auto &triangle = model.triangles()[triangle_index];
    if (triangle.flags == 0 || triangle.degenerate) {
      continue;
    }
    // Lamp fixtures combine an ordinarily lit opaque housing with a
    // semi-transparent emissive halo. Only the halo bypasses scene lighting
    // and depth cue; treating the complete model as fullbright washes out the
    // fixture itself.
    const auto apply_scene_lighting =
        game::objectVisualEffectReceivesSceneLighting(
            visual_effect, triangle.semi_transparent);
    const auto apply_depth_cue = game::objectVisualEffectReceivesDepthCue(
        visual_effect, triangle.semi_transparent);
    const auto transform_vertex = [&](std::uint8_t index) {
      const auto &vertex = model.vertices()[index];
      return visual_effect == game::ObjectVisualEffect::billboard_glow
                 ? transformCylindricalBillboardVertex(model, vertex,
                                                       object.transform, camera)
                 : transformVertex(vertex, object.transform);
    };
    auto v0 = transform_vertex(triangle.vertex_indices[0]);
    auto v1 = transform_vertex(triangle.vertex_indices[1]);
    auto v2 = transform_vertex(triangle.vertex_indices[2]);
    const std::array positions{v0, v1, v2};
    const auto near_status = classifyNearPlane(view, positions);
    const auto two_sided_effect =
        force_two_sided || (model.planar() && triangle.semi_transparent);
    if (near_status == NearPlaneStatus::outside) {
      ++stats.rejected;
      continue;
    }
    if (near_status == NearPlaneStatus::intersecting) {
      const auto normal = visibleSurfaceNormal(positions);
      const std::array textured{
          makeTexturedVertex(v0, triangle.uv[0], back_color,
                             apply_scene_lighting, normal),
          makeTexturedVertex(v1, triangle.uv[1], back_color,
                             apply_scene_lighting, normal),
          makeTexturedVertex(v2, triangle.uv[2], back_color,
                             apply_scene_lighting, normal),
      };
      submitClippedTriangle(
          textured,
          TexturedMaterial{
              triangle.texture_page,
              triangle.clut,
              triangle.semi_transparent || force_semi_transparent,
              false,
              static_cast<std::uint8_t>(texture_bank),
          },
          two_sided_effect, reverse_winding, view, ordering_table,
          primitives.objects, stats, worldOrderingDepth, false,
          apply_depth_cue);
      continue;
    }

    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    const auto visible_face = frontFacing(xy0, xy1, xy2, 3U) != reverse_winding;
    if (depth <= 0 || (!two_sided_effect && !visible_face) ||
        triviallyOutsideViewport(std::array<long, 3U>{xy0, xy1, xy2})) {
      ++stats.rejected;
      continue;
    }

    auto &primitive = primitives.objects.emplace_back();
    configurePrimitive(primitive, triangle, texture_bank, back_color);
    if (force_semi_transparent) {
      setSemiTrans(&primitive, 1);
    }
    if (apply_scene_lighting) {
      dynamicallyLight(primitive, positions);
    }
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    if (apply_depth_cue && !triangle.semi_transparent &&
        !force_semi_transparent) {
      depthCuePrimitive(primitive,
                        {retailObjectDepthCue(cameraDepth(view, v0)),
                         retailObjectDepthCue(cameraDepth(view, v1)),
                         retailObjectDepthCue(cameraDepth(view, v2))});
    }
    addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

struct ActorWeaponPresentation {
  const assets::GmdModel *geometry{};
  assets::MissionTransform transform{};
};

[[nodiscard]] std::optional<ActorWeaponPresentation>
prepareActorWeaponPresentation(const game::GameplaySession &gameplay,
                               const assets::HmdModel &actor_model,
                               const game::SceneObject &actor_object,
                               const game::ActorPose &actor_pose,
                               game::WeaponId weapon,
                               HmdPoseScratch &pose_scratch,
                               const ResolvedHmdPose *prepared_pose) {
  const auto *weapon_model = gameplay.weaponModel(weapon);
  if (weapon_model == nullptr) {
    return std::nullopt;
  }
  const auto *geometry = std::get_if<assets::GmdModel>(&weapon_model->geometry);
  if (geometry == nullptr) {
    return std::nullopt;
  }
  const auto transform =
      posedActorPartTransform(actor_model, actor_object, actor_pose,
                              pose_scratch, "RightHan", prepared_pose);
  if (!transform) {
    return std::nullopt;
  }
  return ActorWeaponPresentation{geometry, *transform};
}

void renderActorWeapon(const ActorWeaponPresentation &presentation,
                       const game::SceneObject &actor_object,
                       unsigned int texture_bank,
                       const game::CameraState &camera, const MATRIX &view,
                       std::vector<OT_TAG> &ordering_table,
                       PrimitiveBuffer &primitives, RenderStats &stats,
                       bool force_semi_transparent = false) {
  auto weapon_object = game::SceneObject{.transform = presentation.transform};
  weapon_object.legacy_hmd_back_color_q12 =
      actor_object.legacy_hmd_back_color_q12;
  weapon_object.legacy_hmd_back_color_valid =
      actor_object.legacy_hmd_back_color_valid;
  renderGmdObject(
      *presentation.geometry, weapon_object, game::ObjectVisualEffect::none,
      texture_bank, camera, view, ordering_table, primitives, stats,
      retailHmdBackColor(actor_object), false, force_semi_transparent);
}

const ResolvedHmdPose *prepareHmdWorldVertices(
    const assets::HmdModel &model, const game::SceneObject &object,
    const game::ActorPose *animation_pose, HmdPoseScratch &pose_scratch,
    std::vector<Vector3> &result) {
  const auto direct_legacy_pose = hasLegacyHmdBones(model, object);
  const auto *resolved_pose =
      direct_legacy_pose ? nullptr
                         : &resolveHmdPose(model, animation_pose, pose_scratch);
  result.clear();
  result.reserve(model.vertices().size());
  for (std::size_t index = 0U; index < model.vertices().size(); ++index) {
    if (!direct_legacy_pose) {
      const auto &source = model.vertices()[index];
      auto vertex = applyHmdPose(
          resolved_pose->parts[model.vertexParts()[index]],
          Vector3{static_cast<double>(source.x), static_cast<double>(source.y),
                  static_cast<double>(source.z)});
      if (!object.legacy_hmd_root_space) {
        vertex.y -= resolved_pose->ground_y;
      }
      result.push_back(
          transformPoint(vertex.x, vertex.y, vertex.z, object.transform));
      continue;
    }
    const auto &source = model.vertices()[index];
    result.push_back(transformPoint(
        static_cast<double>(source.x), static_cast<double>(source.y),
        static_cast<double>(source.z),
        object.legacy_hmd_bones[model.vertexParts()[index]]));
  }
  return resolved_pose;
}

void prepareActorShadowGeometry(const assets::HmdModel &body_model,
                                std::span<const Vector3> body_world_vertices,
                                const ActorWeaponPresentation *weapon,
                                PrimitiveBuffer &primitives) {
  auto &vertices = primitives.actor_scratch.shadow_world_vertices;
  auto &triangles = primitives.actor_scratch.shadow_triangles;
  vertices.clear();
  triangles.clear();
  if (body_world_vertices.size() != body_model.vertices().size()) {
    return;
  }

  const auto weapon_vertex_count =
      weapon == nullptr ? std::size_t{} : weapon->geometry->vertices().size();
  const auto weapon_triangle_count =
      weapon == nullptr ? std::size_t{} : weapon->geometry->triangles().size();
  vertices.reserve(body_world_vertices.size() + weapon_vertex_count);
  triangles.reserve(body_model.triangles().size() + weapon_triangle_count);
  vertices.assign(body_world_vertices.begin(), body_world_vertices.end());
  for (const auto &triangle : body_model.triangles()) {
    triangles.push_back(ActorShadowTriangle{{triangle.vertex_indices[0],
                                             triangle.vertex_indices[1],
                                             triangle.vertex_indices[2]}});
  }
  if (weapon == nullptr) {
    return;
  }

  const auto vertex_offset = vertices.size();
  for (const auto &vertex : weapon->geometry->vertices()) {
    vertices.push_back(
        transformPoint(vertex.x, vertex.y, vertex.z, weapon->transform));
  }
  for (const auto &triangle : weapon->geometry->triangles()) {
    if (triangle.flags == 0U || triangle.degenerate ||
        std::ranges::any_of(triangle.vertex_indices, [&](std::uint8_t index) {
          return index >= weapon->geometry->vertices().size();
        })) {
      continue;
    }
    triangles.push_back(
        ActorShadowTriangle{{vertex_offset + triangle.vertex_indices[0],
                             vertex_offset + triangle.vertex_indices[1],
                             vertex_offset + triangle.vertex_indices[2]}});
  }
}

void renderHmdObject(
    const assets::HmdModel &model, const game::SceneObject &object,
    const game::ActorPose *animation_pose, unsigned int texture_bank,
    HmdPoseScratch &pose_scratch, const MATRIX &view,
    std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats,
    std::optional<VertexColor> back_color_override = std::nullopt,
    std::span<const Vector3> prepared_world_vertices = {},
    bool force_semi_transparent = false, bool apply_scene_lighting = true) {
  if (prepared_world_vertices.empty()) {
    prepareHmdWorldVertices(model, object, animation_pose, pose_scratch,
                            primitives.actor_scratch.hmd_world_vertices);
    prepared_world_vertices = primitives.actor_scratch.hmd_world_vertices;
  }
  const auto world_vertices = prepared_world_vertices;
  const auto back_color =
      back_color_override.value_or(retailHmdBackColor(object));
  const auto vertex = [&](std::size_t index) {
    const auto &world = world_vertices[index];
    return makeVertex(world.x, world.y, world.z);
  };
  for (const auto &triangle : model.triangles()) {
    auto v0 = vertex(triangle.vertex_indices[0]);
    auto v1 = vertex(triangle.vertex_indices[1]);
    auto v2 = vertex(triangle.vertex_indices[2]);
    const std::array positions{v0, v1, v2};
    const auto near_status = classifyNearPlane(view, positions);
    if (near_status == NearPlaneStatus::outside) {
      ++stats.rejected;
      continue;
    }
    if (near_status == NearPlaneStatus::intersecting) {
      const auto normal = visibleSurfaceNormal(positions);
      const std::array textured{
          makeTexturedVertex(v0, triangle.uv[0], back_color,
                             apply_scene_lighting, normal),
          makeTexturedVertex(v1, triangle.uv[1], back_color,
                             apply_scene_lighting, normal),
          makeTexturedVertex(v2, triangle.uv[2], back_color,
                             apply_scene_lighting, normal),
      };
      submitClippedTriangle(
          textured,
          TexturedMaterial{triangle.texture_page, triangle.clut,
                           force_semi_transparent, false,
                           static_cast<std::uint8_t>(texture_bank)},
          true, false, view, ordering_table, primitives.objects, stats);
      continue;
    }

    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    if (depth <= 0 ||
        triviallyOutsideViewport(std::array<long, 3U>{xy0, xy1, xy2})) {
      ++stats.rejected;
      continue;
    }
    auto &primitive = primitives.objects.emplace_back();
    configurePrimitive(primitive, triangle, texture_bank, back_color);
    if (force_semi_transparent) {
      setSemiTrans(&primitive, 1);
    }
    if (apply_scene_lighting) {
      dynamicallyLight(primitive, positions);
    }
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    if (!force_semi_transparent) {
      depthCuePrimitive(primitive,
                        {retailObjectDepthCue(cameraDepth(view, v0)),
                         retailObjectDepthCue(cameraDepth(view, v1)),
                         retailObjectDepthCue(cameraDepth(view, v2))});
    }
    addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

void renderActorShadow(
    const game::GameplaySession &gameplay, const assets::HmdModel &model,
    const game::SceneObject &object, std::span<const Vector3> world_vertices,
    std::span<const ActorShadowTriangle> triangles,
    const void *attachment_geometry, game::DynamicLightPoint support_root,
    std::uint64_t actor_shadow_key, std::uint64_t guest_frame,
    double interpolation_amount, ActorShadowPresentationCache &shadow_cache,
    const MATRIX &view, std::vector<OT_TAG> &shadow_ordering_table,
    PrimitiveBuffer &primitives, RenderStats &stats) {
  if (world_vertices.size() < model.vertices().size() || triangles.empty()) {
    return;
  }
  auto minimum_y = std::numeric_limits<double>::max();
  auto contact_y = std::numeric_limits<double>::lowest();
  for (const auto &triangle : model.triangles()) {
    for (const auto index : triangle.vertex_indices) {
      minimum_y = std::min(minimum_y, world_vertices[index].y);
      contact_y = std::max(contact_y, world_vertices[index].y);
    }
  }
  if (!std::isfinite(minimum_y) || !std::isfinite(contact_y)) {
    return;
  }
  const auto root = transformPoint(0.0, 0.0, 0.0, object.transform);
  const auto actor_anchor =
      game::DynamicLightPoint{root.x, (minimum_y + contact_y) * 0.5, root.z};
  auto &shadow_state = shadow_cache.state(
      actor_shadow_key, guest_frame, &model, attachment_geometry,
      world_vertices.size(), triangles.size() * 3U);
  const auto refresh_shadow_tick =
      !shadow_state.support.initialized ||
      shadow_state.support.guest_tick != guest_frame;
  if (refresh_shadow_tick) {
    auto target_support = std::optional<ActorShadowReceiverPlane>{};
    if (const auto surface = gameplay.actorGroundSurface(
            support_root.x, support_root.z, support_root.y);
        surface && std::abs(surface->y - support_root.y) <= 1200.0) {
      auto support_normal = game::DynamicLightPoint{
          surface->normal_x, surface->normal_y, surface->normal_z};
      if (support_normal.y < 0.0) {
        support_normal = {-support_normal.x, -support_normal.y,
                          -support_normal.z};
      }
      target_support = ActorShadowReceiverPlane{
          {support_root.x, surface->y, support_root.z}, support_normal, false};
    }
    shadow_state.support = advanceActorShadowSupport(
        shadow_state.support, target_support, guest_frame);
    if (shadow_state.support.initialized) {
      const auto authoritative_anchor = game::DynamicLightPoint{
          support_root.x, support_root.y + (actor_anchor.y - root.y),
          support_root.z};
      const auto target_projection = game::selectDynamicShadowProjection(
          active_dynamic_shadow_lights, authoritative_anchor,
          shadow_state.support.current.normal);
      shadow_state.projection = game::advanceDynamicShadowProjection(
          shadow_state.projection, target_projection, guest_frame);
    }
  }
  const auto support =
      sampleActorShadowSupport(shadow_state.support, interpolation_amount);
  if (!support) {
    return;
  }
  const auto ground_point = support->point;
  const auto ground_normal = support->normal;
  const auto projection = game::sampleDynamicShadowProjection(
      shadow_state.projection, interpolation_amount);
  const auto refresh_receivers =
      shadow_state.receiver_guest_frame != guest_frame;
  const auto receiver_for = [&](game::DynamicLightPoint source,
                                ActorShadowCachedReceiver &history)
      -> std::optional<ActorShadowReceiverVertex> {
    const auto floor = game::projectDynamicShadowPoint(
        source, ground_point, ground_normal, projection);
    if (!floor) {
      return std::nullopt;
    }
    if (refresh_receivers) {
      const auto query_floor = game::projectDynamicShadowPoint(
          source, shadow_state.support.current.point,
          shadow_state.support.current.normal, shadow_state.projection.current);
      const auto receiver = gameplay.actorShadowSurface(
          source.x, source.y, source.z, query_floor ? query_floor->x : floor->x,
          query_floor ? query_floor->y : floor->y,
          query_floor ? query_floor->z : floor->z);
      auto candidate = std::optional<ActorShadowReceiverPlane>{};
      if (receiver) {
        const auto receiver_normal = game::DynamicLightPoint{
            receiver->normal_x, receiver->normal_y, receiver->normal_z};
        if (actorShadowReceiverIsWall(receiver_normal, ground_normal)) {
          candidate = ActorShadowReceiverPlane{
              {receiver->x, receiver->y, receiver->z}, receiver_normal, true};
        }
      }
      updateActorShadowReceiver(history, candidate);
    }
    if (history.stable) {
      if (const auto point = projectActorShadowOntoCachedPlane(
              source, *floor, *history.stable)) {
        return ActorShadowReceiverVertex{*point, history.stable->normal,
                                         history.stable->wall};
      }
    }
    return ActorShadowReceiverVertex{*floor, ground_normal, false};
  };
  auto &receivers = primitives.actor_scratch.shadow_receivers;
  receivers.clear();
  receivers.reserve(world_vertices.size());
  for (auto index = std::size_t{}; index < world_vertices.size(); ++index) {
    const auto &vertex = world_vertices[index];
    receivers.push_back(receiver_for({vertex.x, vertex.y, vertex.z},
                                     shadow_state.receivers[index]));
  }
  shadow_state.receiver_guest_frame = guest_frame;
  const auto shade = static_cast<std::uint8_t>(std::clamp<long>(
      std::lround(18.0 + projection.darkness * 58.0), 20L, 44L));
  const auto projected_valid = [](long packed) {
    constexpr auto maximum_safe_coordinate = 8192.0F;
    return std::isfinite(screenX(packed)) && std::isfinite(screenY(packed)) &&
           std::abs(screenX(packed)) <= maximum_safe_coordinate &&
           std::abs(screenY(packed)) <= maximum_safe_coordinate;
  };
  const auto submit_shadow_triangle = [&](game::DynamicLightPoint first,
                                          game::DynamicLightPoint second,
                                          game::DynamicLightPoint third) {
    auto v0 = makeVertex(first.x, first.y, first.z);
    auto v1 = makeVertex(second.x, second.y, second.z);
    auto v2 = makeVertex(third.x, third.y, third.z);
    const std::array positions{v0, v1, v2};
    if (classifyNearPlane(view, positions) != NearPlaneStatus::inside) {
      return;
    }
    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    if (depth <= 0 || !projected_valid(xy0) || !projected_valid(xy1) ||
        !projected_valid(xy2)) {
      return;
    }
    const auto area =
        (screenX(xy1) - screenX(xy0)) * (screenY(xy2) - screenY(xy0)) -
        (screenY(xy1) - screenY(xy0)) * (screenX(xy2) - screenX(xy0));
    if (!std::isfinite(area) || std::abs(area) < 0.35F) {
      return;
    }
    auto &primitive = primitives.actor_shadows.emplace_back();
    setPolyF3(&primitive);
    setSemiTrans(&primitive, 1);
    setRGB0(&primitive, shade, shade, shade);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    addPrim(
        &shadow_ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
        &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  };
  const auto midpoint = [](game::DynamicLightPoint first,
                           game::DynamicLightPoint second) {
    return game::DynamicLightPoint{(first.x + second.x) * 0.5,
                                   (first.y + second.y) * 0.5,
                                   (first.z + second.z) * 0.5};
  };
  const auto normal_dot = [](const ActorShadowReceiverVertex &first,
                             const ActorShadowReceiverVertex &second) {
    return first.normal.x * second.normal.x + first.normal.y * second.normal.y +
           first.normal.z * second.normal.z;
  };
  for (auto triangle_index = std::size_t{}; triangle_index < triangles.size();
       ++triangle_index) {
    const auto &triangle = triangles[triangle_index];
    if (std::ranges::any_of(triangle.vertex_indices, [&](std::size_t index) {
          return index >= world_vertices.size() || !receivers[index];
        })) {
      continue;
    }
    const auto first_index = triangle.vertex_indices[0];
    const auto second_index = triangle.vertex_indices[1];
    const auto third_index = triangle.vertex_indices[2];
    const auto &first = *receivers[first_index];
    const auto &second = *receivers[second_index];
    const auto &third = *receivers[third_index];
    const auto receiver_changes =
        first.wall != second.wall || first.wall != third.wall ||
        second.wall != third.wall ||
        (first.wall && (normal_dot(first, second) < 0.985 ||
                        normal_dot(first, third) < 0.985 ||
                        normal_dot(second, third) < 0.985));
    if (!receiver_changes) {
      submit_shadow_triangle(first.point, second.point, third.point);
      continue;
    }

    // Subdivide only where one posed triangle crosses a floor/wall crease.
    // Midpoint receiver samples keep the bend stable while preserving the
    // detailed HMD outline everywhere else.
    const auto first_source = game::DynamicLightPoint{
        world_vertices[first_index].x, world_vertices[first_index].y,
        world_vertices[first_index].z};
    const auto second_source = game::DynamicLightPoint{
        world_vertices[second_index].x, world_vertices[second_index].y,
        world_vertices[second_index].z};
    const auto third_source = game::DynamicLightPoint{
        world_vertices[third_index].x, world_vertices[third_index].y,
        world_vertices[third_index].z};
    const auto edge_offset = triangle_index * 3U;
    const auto first_second =
        receiver_for(midpoint(first_source, second_source),
                     shadow_state.edge_receivers[edge_offset]);
    const auto second_third =
        receiver_for(midpoint(second_source, third_source),
                     shadow_state.edge_receivers[edge_offset + 1U]);
    const auto third_first =
        receiver_for(midpoint(third_source, first_source),
                     shadow_state.edge_receivers[edge_offset + 2U]);
    if (!first_second || !second_third || !third_first) {
      submit_shadow_triangle(first.point, second.point, third.point);
      continue;
    }
    submit_shadow_triangle(first.point, first_second->point,
                           third_first->point);
    submit_shadow_triangle(first_second->point, second.point,
                           second_third->point);
    submit_shadow_triangle(third_first->point, second_third->point,
                           third.point);
    submit_shadow_triangle(first_second->point, second_third->point,
                           third_first->point);
  }
}
