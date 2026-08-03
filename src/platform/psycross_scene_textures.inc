class RetailCrateTextureOverlay final {
public:
  explicit RetailCrateTextureOverlay(const game::MissionPackage &mission)
      : trim_(assets::TimImage::parse(
            mission.interfaceAssets().file("WEPTRIM.TIM"))),
        inner_(assets::TimImage::parse(
            mission.interfaceAssets().file("WEPINNER.TIM"))),
        top_(assets::TimImage::parse(
            mission.interfaceAssets().file("WEPTOP.TIM"))) {
    const auto valid = [](const assets::TimImage &image) {
      return image.mode() == assets::TimPixelMode::indexed8 && image.clut() &&
             image.clut()->x == mission_clut_source_x &&
             image.clut()->y == mission_clut_source_y + 3U &&
             image.clut()->width_words == 256U && image.clut()->height == 1U;
    };
    if (!valid(trim_) || !valid(inner_) || !valid(top_) ||
        trim_.clut()->words != inner_.clut()->words ||
        trim_.clut()->words != top_.clut()->words ||
        timTexturePage(trim_) != 12U || timTexturePage(inner_) != 13U ||
        timTexturePage(top_) != 13U) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Retail weapon-crate texture layout is invalid"};
    }
  }

  [[nodiscard]] static constexpr std::size_t sourceClutRow() noexcept {
    return 3U;
  }

  void copyClut(std::span<std::byte> destination) const {
    constexpr std::size_t row_bytes = 256U * 2U;
    if (destination.size() != row_bytes) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Weapon-crate CLUT destination is invalid"};
    }
    const auto &words = trim_.clut()->words;
    for (std::size_t index = 0U; index < words.size(); ++index) {
      destination[index * 2U] = static_cast<std::byte>(words[index] & 0xffU);
      destination[index * 2U + 1U] = static_cast<std::byte>(words[index] >> 8U);
    }
  }

  void upload(unsigned int trim_page, unsigned int top_page) const {
    uploadAt(trim_, trim_page);
    uploadAt(inner_, top_page);
    uploadAt(top_, top_page);
  }

private:
  static void uploadAt(const assets::TimImage &image,
                       unsigned int physical_page) {
    if (physical_page >= resident_texture_page_count) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Weapon-crate texture page is invalid"};
    }
    const auto source_page = timTexturePage(image);
    const auto source_page_x = (source_page & 15U) * 64U;
    const auto source_page_y = source_page > 15U ? 256U : 0U;
    uploadTexturePageBlockAt(physical_page, image.pixels(),
                             image.pixels().x - source_page_x,
                             image.pixels().y - source_page_y);
  }

  assets::TimImage trim_;
  assets::TimImage inner_;
  assets::TimImage top_;
};

struct EffectSpritePlacement {
  std::uint16_t texture_page{};
  std::uint16_t clut{};
  double minimum_u{};
  double minimum_v{};
  double maximum_u{};
  double maximum_v{};
};

struct FireTexturePlacement {
  bool valid{};
  std::array<std::array<EffectSpritePlacement, 16U>, 4U> frames{};

  [[nodiscard]] const EffectSpritePlacement *
  frame(game::LegacyEffectSpriteFamily family,
        std::size_t frame_index) const noexcept {
    const auto family_index = static_cast<std::size_t>(family);
    if (!valid || family_index == 0U || family_index > frames.size() ||
        frame_index >= game::legacyEffectSpriteLayout(family).frame_count) {
      return nullptr;
    }
    return &frames[family_index - 1U][frame_index];
  }
};

class CombatEffectTextureAtlas final {
public:
  explicit CombatEffectTextureAtlas(const game::MissionPackage &mission)
      : blood_(
            assets::TimImage::parse(mission.specialEffects().file("BLOT.TIM"))),
        blood_mark_(assets::TimImage::parse(
            mission.specialEffects().file("BBHOLE.TIM"))) {
    validate(blood_, "BLOT.TIM");
    validate(blood_mark_, "BBHOLE.TIM");
    if (blood_.clut()->words != blood_mark_.clut()->words) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "SPFX combat sprites do not share retail palettes"};
    }
    restore();
  }

  void restore() const {
    uploadMuzzleFlash();
    upload(blood_, 80U, effect_resident_secondary_clut_y);
    upload(blood_mark_, 96U, effect_resident_secondary_clut_y);
    DrawSync(0);
  }

  [[nodiscard]] EffectSpritePlacement muzzleFlash() const noexcept {
    return EffectSpritePlacement{
        static_cast<std::uint16_t>(
            GetTPage(0, 1, effect_resident_page_x, effect_resident_page_y)),
        static_cast<std::uint16_t>(
            GetClut(effect_resident_clut_x, muzzle_flash_resident_clut_y)),
        static_cast<double>(muzzle_flash_resident_u),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y),
        static_cast<double>(muzzle_flash_resident_u +
                            muzzle_flash_texture::width - 1U),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y +
                            muzzle_flash_texture::height - 1U),
    };
  }
  [[nodiscard]] EffectSpritePlacement blood() const noexcept {
    return placement(80U, effect_resident_secondary_clut_y, blood_);
  }
  [[nodiscard]] EffectSpritePlacement bloodMark() const noexcept {
    return placement(96U, effect_resident_secondary_clut_y, blood_mark_);
  }

private:
  static void uploadMuzzleFlash() {
    static_assert(muzzle_flash_texture::pixels.size() ==
                  muzzle_flash_texture::width_words *
                      muzzle_flash_texture::height);
    RECT16 pixels{
        static_cast<short>(effect_resident_page_x +
                           muzzle_flash_resident_u / 4U),
        static_cast<short>(effect_resident_vram_y),
        static_cast<short>(muzzle_flash_texture::width_words),
        static_cast<short>(muzzle_flash_texture::height),
    };
    LoadImage(&pixels, reinterpret_cast<u_long *>(const_cast<std::uint16_t *>(
                           muzzle_flash_texture::pixels.data())));
    RECT16 clut{
        static_cast<short>(effect_resident_clut_x),
        static_cast<short>(muzzle_flash_resident_clut_y),
        static_cast<short>(muzzle_flash_texture::clut.size()),
        1,
    };
    LoadImage(&clut, reinterpret_cast<u_long *>(const_cast<std::uint16_t *>(
                         muzzle_flash_texture::clut.data())));
  }

  static void validate(const assets::TimImage &image, std::string_view name) {
    if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
        image.clut()->width_words != 256U || image.clut()->height != 1U ||
        image.displayWidth() > 32U || image.displayHeight() > 64U) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Unexpected combat effect TIM layout: " +
                            std::string{name}};
    }
  }

  static void upload(const assets::TimImage &image, std::uint16_t u,
                     std::uint16_t clut_y) {
    uploadTimBlockAt(
        image.pixels(),
        static_cast<std::uint16_t>(effect_resident_page_x + u / 2U),
        effect_resident_vram_y);
    uploadTimBlockAt(*image.clut(), effect_resident_clut_x, clut_y);
  }

  static EffectSpritePlacement
  placement(std::uint16_t u, std::uint16_t clut_y,
            const assets::TimImage &image) noexcept {
    const auto width = image.displayWidth();
    const auto height = image.displayHeight();
    return EffectSpritePlacement{
        static_cast<std::uint16_t>(
            GetTPage(1, 1, effect_resident_page_x, effect_resident_page_y)),
        static_cast<std::uint16_t>(GetClut(effect_resident_clut_x, clut_y)),
        static_cast<double>(u),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y),
        static_cast<double>(u + width - 1U),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y +
                            height - 1U),
    };
  }

  assets::TimImage blood_;
  assets::TimImage blood_mark_;
};

FireTexturePlacement
uploadFireTextureAtlas(const game::ObjectFireEmitter &fire) {
  if (fire.frames.empty() || fire.fire_frames.empty() ||
      fire.breath_frames.empty() || fire.vapor_frames.empty() ||
      !fire.frames.front().clut()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "CFIRE texture set is empty"};
  }
  auto placement = FireTexturePlacement{true};
  const auto upload_family = [&](const std::vector<assets::TimImage> &frames,
                                 game::LegacyEffectSpriteFamily family,
                                 unsigned int physical_page,
                                 std::uint16_t base_y, std::size_t columns) {
    const auto layout = game::legacyEffectSpriteLayout(family);
    if (frames.size() != layout.frame_count || columns == 0U) {
      throw core::Error{core::ErrorCode::unsupported,
                        "SPFX animation has an unexpected frame count"};
    }
    const auto page_x = static_cast<std::uint16_t>((physical_page & 15U) * 64U);
    const auto page_y =
        static_cast<std::uint16_t>(physical_page > 15U ? 256U : 0U);
    for (std::size_t index = 0; index < frames.size(); ++index) {
      const auto &frame = frames[index];
      const auto &pixels = frame.pixels();
      if (frame.mode() != assets::TimPixelMode::indexed8 ||
          pixels.width_words != layout.width_words ||
          pixels.height != layout.height || !frame.clut() ||
          frame.clut()->words != fire.frames.front().clut()->words) {
        throw core::Error{core::ErrorCode::unsupported,
                          "SPFX frame does not match its retail family layout"};
      }
      const auto column = index % columns;
      const auto row = index / columns;
      const auto local_x = column * layout.width_words;
      const auto local_y =
          static_cast<std::size_t>(base_y) + row * layout.height;
      if (local_x + layout.width_words > 64U ||
          local_y + layout.height > 256U) {
        throw core::Error{core::ErrorCode::unsupported,
                          "Native SPFX atlas is exhausted"};
      }
      const auto destination_x = static_cast<std::uint16_t>(page_x + local_x);
      const auto destination_y = static_cast<std::uint16_t>(page_y + local_y);
      uploadTimBlockAt(pixels, destination_x, destination_y);
      const auto minimum_u = static_cast<double>(local_x * 2U);
      const auto minimum_v = static_cast<double>(local_y);
      placement.frames[static_cast<std::size_t>(family) - 1U][index] =
          EffectSpritePlacement{
              static_cast<std::uint16_t>(
                  GetTPage(texturePageMode(frame.mode()), 1, page_x, page_y)),
              static_cast<std::uint16_t>(
                  GetClut(fire_resident_clut_x, fire_resident_clut_y)),
              minimum_u,
              minimum_v,
              minimum_u + static_cast<double>(frame.displayWidth() - 1U),
              minimum_v + static_cast<double>(frame.displayHeight() - 1U),
          };
    }
  };
  upload_family(fire.fire_frames, game::LegacyEffectSpriteFamily::fire,
                fire_resident_fire_page, 0U, 4U);
  upload_family(fire.frames, game::LegacyEffectSpriteFamily::explosion,
                fire_resident_other_page, 0U, 4U);
  upload_family(fire.vapor_frames, game::LegacyEffectSpriteFamily::vapor,
                fire_resident_other_page, 96U, 4U);
  // BRETH frames are the only 16-pixel-wide indexed8 family: their TIM
  // payload is eight 16-bit words per row, not the 16 used by 32px families.
  upload_family(fire.breath_frames, game::LegacyEffectSpriteFamily::breath,
                fire_resident_other_page, 160U, 8U);
  uploadTimBlockAt(*fire.frames.front().clut(), fire_resident_clut_x,
                   fire_resident_clut_y);
  return placement;
}

struct GuestSpriteTexturePlacement {
  std::uint16_t texture_page{};
  std::uint16_t clut{};
  std::uint8_t u{};
  std::uint8_t v{};
};

struct GlassShardVertexState {
  double x{};
  double y{};
  double z{};
  double u{};
  double v{};
};

struct GlassShardVectorState {
  double x{};
  double y{};
  double z{};
};

struct GlassShardPresentationState {
  std::array<GlassShardVertexState, 3U> vertices{};
  GlassShardVectorState centre{};
  GlassShardVectorState velocity{};
  GlassShardVectorState spin_axis{0.0, 1.0, 0.0};
  double angular_speed{};
  std::uint16_t texture_page{};
  std::uint16_t clut{};
  std::uint8_t texture_bank{};
  bool semi_transparent{};
  std::uint16_t age{};
  std::uint16_t lifetime{28U};
};

class EnvironmentTextureAtlas final {
public:
  explicit EnvironmentTextureAtlas(const game::MissionPackage &mission) {
    const auto append = [&](std::string_view name, std::uint16_t u,
                            std::uint16_t v) {
      auto image = assets::TimImage::parse(mission.specialEffects().file(name));
      if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
          image.clut()->width_words != 256U || image.clut()->height != 1U ||
          static_cast<unsigned int>(u) + image.displayWidth() > 256U ||
          static_cast<unsigned int>(v) + image.displayHeight() > 256U) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "Unexpected retail environment TIM layout: " +
                              std::string{name}};
      }
      if (!entries_.empty() &&
          image.clut()->words != entries_.front().image.clut()->words) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "Retail environment TIM files do not share the SPFX palette"};
      }
      entries_.push_back(Entry{std::move(image), u, v});
    };
    for (std::size_t index = 0; index < 8U; ++index) {
      auto name = std::string{"STEAM00"} + std::to_string(index) + ".TIM";
      append(name, static_cast<std::uint16_t>((index % 4U) * 32U),
             static_cast<std::uint16_t>((index / 4U) * 64U));
    }
    append("SNOPRTR.TIM", 0U, 128U);
    append("SNOPRTL.TIM", 32U, 128U);
    for (std::size_t index = 0; index < 16U; ++index) {
      auto name = std::string{"BRETH"};
      if (index < 10U) {
        name.push_back('0');
      }
      name += std::to_string(index) + ".TIM";
      append(name, static_cast<std::uint16_t>((index % 8U) * 16U),
             static_cast<std::uint16_t>(160U + (index / 8U) * 16U));
    }
  }

  [[nodiscard]] bool
  contains(const game::LegacyGuestSpriteBridgeState &sprite) const noexcept {
    return find(sprite) != nullptr;
  }

  [[nodiscard]] std::optional<GuestSpriteTexturePlacement>
  relocate(const game::LegacyGuestSpriteBridgeState &sprite,
           unsigned int physical_page) const noexcept {
    const auto *entry = find(sprite);
    if (entry == nullptr || physical_page >= resident_texture_page_count) {
      return std::nullopt;
    }
    const auto source = sourcePlacement(entry->image);
    const auto local_u = static_cast<unsigned int>(sprite.u) - source.u;
    const auto local_v = static_cast<unsigned int>(sprite.v) - source.v;
    return GuestSpriteTexturePlacement{
        encodeResidentTexturePage(
            static_cast<std::uint16_t>(GetTPage(1, 0, 0, 0)), physical_page),
        static_cast<std::uint16_t>(
            GetClut(effect_resident_clut_x, environment_resident_clut_y)),
        static_cast<std::uint8_t>(entry->destination_u + local_u),
        static_cast<std::uint8_t>(entry->destination_v + local_v),
    };
  }

  void upload(unsigned int physical_page) const {
    if (physical_page >= resident_texture_page_count) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Environment atlas page is invalid"};
    }
    for (const auto &entry : entries_) {
      uploadTexturePageBlockAt(physical_page, entry.image.pixels(),
                               entry.destination_u / 2U, entry.destination_v);
    }
    uploadTimBlockAt(*entries_.front().image.clut(), effect_resident_clut_x,
                     environment_resident_clut_y);
  }

private:
  struct SourcePlacement {
    unsigned int page{};
    unsigned int u{};
    unsigned int v{};
    unsigned int clut_x{};
    unsigned int clut_y{};
  };

  struct Entry {
    assets::TimImage image;
    std::uint16_t destination_u{};
    std::uint16_t destination_v{};
  };

  [[nodiscard]] static SourcePlacement
  sourcePlacement(const assets::TimImage &image) noexcept {
    const auto page = timTexturePage(image);
    const auto page_x = (page & 15U) * 64U;
    const auto page_y = page > 15U ? 256U : 0U;
    return SourcePlacement{
        page,
        (static_cast<unsigned int>(image.pixels().x) - page_x) * 2U,
        static_cast<unsigned int>(image.pixels().y) - page_y,
        image.clut()->x,
        image.clut()->y,
    };
  }

  [[nodiscard]] const Entry *
  find(const game::LegacyGuestSpriteBridgeState &sprite) const noexcept {
    const auto match = std::ranges::find_if(entries_, [&](const Entry &entry) {
      const auto source = sourcePlacement(entry.image);
      return (sprite.tpage & 0x1fU) == source.page &&
             sprite.center_x == source.clut_x &&
             sprite.center_y == source.clut_y && sprite.u >= source.u &&
             sprite.v >= source.v &&
             static_cast<unsigned int>(sprite.u) + sprite.width <=
                 source.u + entry.image.displayWidth() &&
             static_cast<unsigned int>(sprite.v) + sprite.height <=
                 source.v + entry.image.displayHeight();
    });
    return match == entries_.end() ? nullptr : &*match;
  }

  std::vector<Entry> entries_;
};

struct FireGuestSpriteMatch {
  game::LegacyEffectSpriteFamily family{};
  std::size_t frame{};
  unsigned int source_u{};
  unsigned int source_v{};
};

[[nodiscard]] bool timContainsGuestSprite(
    const assets::TimImage &image,
    const game::LegacyGuestSpriteBridgeState &sprite) noexcept {
  if (!image.clut()) {
    return false;
  }
  const auto page = timTexturePage(image);
  const auto page_x = (page & 15U) * 64U;
  const auto page_y = page > 15U ? 256U : 0U;
  const auto pixels_per_word =
      image.mode() == assets::TimPixelMode::indexed4   ? 4U
      : image.mode() == assets::TimPixelMode::indexed8 ? 2U
                                                       : 1U;
  const auto source_u =
      (static_cast<unsigned int>(image.pixels().x) - page_x) * pixels_per_word;
  const auto source_v = static_cast<unsigned int>(image.pixels().y) - page_y;
  return (sprite.tpage & 0x1fU) == page && sprite.center_x == image.clut()->x &&
         sprite.center_y == image.clut()->y && sprite.u >= source_u &&
         sprite.v >= source_v &&
         static_cast<unsigned int>(sprite.u) + sprite.width <=
             source_u + image.displayWidth() &&
         static_cast<unsigned int>(sprite.v) + sprite.height <=
             source_v + image.displayHeight();
}

[[nodiscard]] std::optional<FireGuestSpriteMatch> matchFireGuestSprite(
    const game::ObjectFireEmitter &fire,
    const game::LegacyGuestSpriteBridgeState &sprite) noexcept {
  const auto match_family = [&](const std::vector<assets::TimImage> &frames,
                                game::LegacyEffectSpriteFamily family)
      -> std::optional<FireGuestSpriteMatch> {
    for (std::size_t index = 0; index < frames.size(); ++index) {
      if (!timContainsGuestSprite(frames[index], sprite)) {
        continue;
      }
      const auto page = timTexturePage(frames[index]);
      return FireGuestSpriteMatch{
          family,
          index,
          (static_cast<unsigned int>(frames[index].pixels().x) -
           (page & 15U) * 64U) *
              2U,
          static_cast<unsigned int>(frames[index].pixels().y) -
              (page > 15U ? 256U : 0U),
      };
    }
    return std::nullopt;
  };
  if (sprite.effect_family != 0U) {
    const auto family =
        static_cast<game::LegacyEffectSpriteFamily>(sprite.effect_family);
    const auto *frames = [&]() -> const std::vector<assets::TimImage> * {
      switch (family) {
      case game::LegacyEffectSpriteFamily::fire:
        return &fire.fire_frames;
      case game::LegacyEffectSpriteFamily::explosion:
        return &fire.frames;
      case game::LegacyEffectSpriteFamily::breath:
        return &fire.breath_frames;
      case game::LegacyEffectSpriteFamily::vapor:
        return &fire.vapor_frames;
      }
      return nullptr;
    }();
    if (frames == nullptr || sprite.effect_frame >= frames->size() ||
        !timContainsGuestSprite((*frames)[sprite.effect_frame], sprite)) {
      return std::nullopt;
    }
    const auto page = timTexturePage((*frames)[sprite.effect_frame]);
    return FireGuestSpriteMatch{
        family,
        sprite.effect_frame,
        (static_cast<unsigned int>((*frames)[sprite.effect_frame].pixels().x) -
         (page & 15U) * 64U) *
            2U,
        static_cast<unsigned int>((*frames)[sprite.effect_frame].pixels().y) -
            (page > 15U ? 256U : 0U),
    };
  }
  if (const auto match = match_family(fire.fire_frames,
                                      game::LegacyEffectSpriteFamily::fire)) {
    return match;
  }
  if (const auto match = match_family(
          fire.frames, game::LegacyEffectSpriteFamily::explosion)) {
    return match;
  }
  if (const auto match = match_family(fire.breath_frames,
                                      game::LegacyEffectSpriteFamily::breath)) {
    return match;
  }
  return match_family(fire.vapor_frames, game::LegacyEffectSpriteFamily::vapor);
}

std::uint32_t uploadVlf(const game::MissionPackage &mission) {
  const auto bytes = mission.archive().file("VLF.RFF");
  const auto page_mask = validateVlf(bytes);
  streamed_vlf_page_mask = page_mask;

  for (unsigned int page = 0; page < 32U; ++page) {
    if ((page_mask & (1U << page)) == 0) {
      continue;
    }
    uploadTexturePage(page, vlfPage(bytes, page_mask, page));
  }
  uploadClut(vlfClut(bytes, page_mask));
  DrawSync(0);
  return page_mask;
}

std::string texturePageName(unsigned int page) {
  std::string result = "TP";
  if (page < 10U) {
    result.push_back('0');
  }
  result += std::to_string(page);
  result += ".BIN";
  return result;
}

bool hasLegacyHmdBones(const assets::HmdModel &model,
                       const game::SceneObject &object) noexcept;

class TextureStreamer final {
public:
  explicit TextureStreamer(const game::MissionPackage &mission)
      : mission_(mission), environment_atlas_(mission),
        crate_texture_overlay_(mission) {
    page_mask_ = uploadVlf(mission_);
    const auto vlf = mission_.archive().file("VLF.RFF");
    const auto initial_clut = vlfClut(vlf, page_mask_);
    loaded_clut_.assign(initial_clut.begin(), initial_clut.end());
    const auto texture_bank_count = mission_.textureBankCount();
    if (texture_bank_count == 0U) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Mission has no texture bank"};
    }
    for (auto page = 0U; page < 32U; ++page) {
      const auto name = texturePageName(page);
      const auto bank_zero = mission_.textureBank(0U).file(name);
      texture_banks_equivalent_[page] = texture_bank_count == 1U;
      if (texture_bank_count > 1U) {
        const auto bank_one = mission_.textureBank(1U).file(name);
        texture_banks_equivalent_[page] =
            std::ranges::equal(bank_zero, bank_one);
      }
      if ((page_mask_ & (1U << page)) == 0U) {
        continue;
      }
      for (auto bank = std::size_t{}; bank < vlf_compatible_pages_.size();
           ++bank) {
        const auto source_bank = canonicalMissionTextureBank(
            static_cast<int>(bank), texture_bank_count);
        vlf_compatible_pages_[bank][page] = texturePageMatchesVlf(
            vlf, page_mask_, page,
            mission_.textureBank(static_cast<std::size_t>(source_bank))
                .file(texturePageName(page)));
      }
    }
    for (unsigned int page = 0; page < 32U; ++page) {
      if ((page_mask_ & (1U << page)) != 0) {
        const auto physical_page = physicalTexturePage(page);
        loaded_pages_[physical_page] =
            PageState{static_cast<int>(page), shared_bank};
        ++page_generations_[physical_page];
        page_upload_write_sequences_[physical_page] = GR_GetVRAMWriteSequence();
      }
    }
  }

  void ensure(const game::GameplaySession &gameplay,
              std::span<const game::LegacyGuestSpritePresentationState>
                  retained_sprites = {},
              std::span<const GlassShardPresentationState> glass_shards = {}) {
    const auto &models = gameplay.models();
    const auto vlf = mission_.archive().file("VLF.RFF");
    const auto object_bank =
        static_cast<int>(models[gameplay.currentRoom()].scene.textureBank());
    const auto camera = gameplay.camera();
    const auto guest_sprite_bank =
        static_cast<int>(gameplay.textureBankAt(camera.x, camera.z));
    const auto requirements_rebuilt =
        requirementsChanged(gameplay, object_bank, guest_sprite_bank,
                            retained_sprites, glass_shards);
    if (requirements_rebuilt) {
      required_pages_.fill(unused_bank);
      required_banks_.fill(-1);
      for (auto &sources : required_clut_sources_) {
        sources.fill(false);
      }
      required_active_fire_ = nullptr;
      required_environment_page_.reset();
      required_crate_banks_.fill(false);
      required_crate_overlay_pages_.fill(false);
      required_scrim_destination_pages_.fill(false);
      scrim_destination_page_mask_ = 0U;
      if (const auto frame = gameplay.legacyPresentationFrame();
          frame && frame->renderer &&
          frame->renderer->state.scrim.resource_present) {
        for (const auto &move : frame->renderer->state.scrim.vram_moves) {
          const auto page =
              static_cast<unsigned int>(move.destination_x / 64) +
              static_cast<unsigned int>(move.destination_y / 256) * 16U;
          if (page < 32U) {
            scrim_destination_page_mask_ |= (1U << page);
          }
        }
      }
      for (auto &owners : required_page_owners_) {
        owners.clear();
      }
      auto requires_environment_atlas = false;
      for (auto &bank_remap : required_page_remap_) {
        for (unsigned int page = 0; page < bank_remap.size(); ++page) {
          bank_remap[page] = physicalTexturePage(page);
        }
      }
      for (auto &bank_remap : required_clut_row_remap_) {
        for (unsigned int row = 0; row < bank_remap.size(); ++row) {
          bank_remap[row] = row;
        }
      }
      auto requirement_context = std::string{"unclassified"};
      const auto alias_pages = [] {
        std::array<unsigned int, resident_texture_page_count - 6U> result{};
        constexpr std::array native_order{
            16U, 17U, 18U, 19U, 20U, 21U, 6U,  7U,  8U,  9U,  10U, 11U, 12U,
            13U, 14U, 15U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U,
        };
        std::ranges::copy(native_order, result.begin());
        for (auto extension = 0U; extension < extended_texture_page_count;
             ++extension) {
          result[native_order.size() + extension] =
              psx_texture_page_count + extension;
        }
        return result;
      }();
      const auto allocate_free_page = [&](int logical_page, int bank) {
        // Prefer the previous physical home. Stable residency avoids both a
        // redundant extension upload and texture-page identity changing only
        // because object enumeration order changed at a room boundary.
        const auto reusable =
            std::ranges::find_if(alias_pages, [&](unsigned int candidate) {
              return required_pages_[candidate] < 0 &&
                     loaded_pages_[candidate].logical_page == logical_page &&
                     loaded_pages_[candidate].bank == bank;
            });
        if (reusable != alias_pages.end()) {
          return *reusable;
        }
        const auto available =
            std::ranges::find_if(alias_pages, [&](unsigned int candidate) {
              return required_pages_[candidate] < 0;
            });
        if (available == alias_pages.end()) {
          auto detail = std::string{"Active scene exhausts the VRAM "
                                    "texture alias pool: owner="} +
                        requirement_context + " resident=";
          for (std::size_t resident = 0; resident < required_pages_.size();
               ++resident) {
            if (required_pages_[resident] < 0) {
              continue;
            }
            detail += std::to_string(resident) + ":" +
                      std::to_string(required_pages_[resident]) + "/" +
                      std::to_string(required_banks_[resident]) + ",";
          }
          throw core::Error{core::ErrorCode::unsupported, std::move(detail)};
        }
        return *available;
      };
      const auto append_owner = [&](unsigned int slot) {
        auto &owners = required_page_owners_[slot];
        if (std::ranges::find(owners, requirement_context) == owners.end()) {
          owners.push_back(requirement_context);
        }
      };
      const auto require_page = [&](unsigned int source_page, int bank) {
        if (bank < 0 || bank >= 2) {
          return 0U;
        }
        source_page &= 0x1fU;
        const auto source_bank = texturePageSourceBank(source_page, bank);
        auto slot = physicalTexturePage(source_page);
        const auto canonical_slot_reserved = slot == effect_resident_page;
        if (canonical_slot_reserved ||
            (required_pages_[slot] >= 0 &&
             (required_pages_[slot] != static_cast<int>(source_page) ||
              required_banks_[slot] != source_bank))) {
          auto existing = required_pages_.size();
          for (unsigned int candidate = 0; candidate < required_pages_.size();
               ++candidate) {
            if (required_pages_[candidate] == static_cast<int>(source_page) &&
                required_banks_[candidate] == source_bank) {
              existing = candidate;
              break;
            }
          }
          if (existing != required_pages_.size()) {
            slot = static_cast<unsigned int>(existing);
          } else {
            // A bank conflict may use any currently free mission-safe page.
            // Pages 0..5 remain exact PS1 presentation/effect storage.
            slot =
                allocate_free_page(static_cast<int>(source_page), source_bank);
          }
        }
        required_page_remap_[static_cast<std::size_t>(bank)][source_page] =
            slot;
        if (required_pages_[slot] < 0) {
          required_pages_[slot] = static_cast<int>(source_page);
          required_banks_[slot] = source_bank;
        } else if (required_pages_[slot] != static_cast<int>(source_page) ||
                   required_banks_[slot] != source_bank) {
          throw core::Error{
              core::ErrorCode::unsupported,
              "Texture alias allocation produced an ownership conflict"};
        }
        append_owner(slot);
        return slot;
      };
      const auto require_mask = [&](std::uint32_t mask, int bank) {
        for (unsigned int page = 0; page < 32U; ++page) {
          if ((mask & (1U << page)) != 0U) {
            require_page(page, bank);
          }
        }
      };
      const auto clut_for_bank = [&](int bank) -> std::span<const std::byte> {
        bank = canonicalMissionTextureBank(bank, mission_.textureBankCount());
        if (bank == 0) {
          // VLF carries the complete bank-zero palette and is byte-identical
          // to VRAM.HOG/ZCLUT.BIN for this mission.
          return vlfClut(vlf, page_mask_);
        }
        return mission_.textureBank(static_cast<std::size_t>(bank))
            .file("ZCLUT.BIN");
      };
      const auto require_clut = [&](std::uint16_t clut,
                                    unsigned int source_page, int bank) {
        if (bank < 0 || bank >= 2) {
          return;
        }
        const auto y = static_cast<unsigned int>(clut >> 6U);
        if (y < mission_clut_source_y || y >= mission_clut_source_y + 32U) {
          return;
        }
        static_cast<void>(source_page);
        // VLF is an initial page preload, not a promise that the other
        // texture bank uses the same palette row. Track palette ownership
        // independently; identical rows are merged by the allocator below.
        const auto palette_bank =
            canonicalMissionTextureBank(bank, mission_.textureBankCount());
        const auto row = y - mission_clut_source_y;
        required_clut_sources_[static_cast<std::size_t>(palette_bank)][row] =
            true;
      };
      const auto require_emd_scene = [&](const assets::EmdScene &scene,
                                         int bank) {
        const auto resolved_mask = scene.resolvedTexturePageMask(page_mask_);
        if (!resolved_mask) {
          throw core::Error{
              core::ErrorCode::unsupported,
              "EMD texture selector has ambiguous source-page ownership"};
        }
        // resolvedTexturePageMask validates authored selector ownership. The
        // exact resident set comes from renderable polygons below; requiring
        // the complete authored mask also retained pages used only by culled
        // or malformed records and could exhaust native alias space.
        for (const auto &section : scene.sections()) {
          for (const auto &polygon : section.polygons) {
            if (!polygon.renderable || polygon.degenerate) {
              continue;
            }
            const auto source_page = assets::resolveEmdTexturePageSource(
                polygon.texture_page, scene.texturePageMask(), page_mask_);
            if (!source_page) {
              throw core::Error{
                  core::ErrorCode::unsupported,
                  "EMD texture selector cannot resolve its source page"};
            }
            require_page(*source_page, bank);
            require_clut(polygon.clut, *source_page, bank);
          }
        }
      };
      for (const auto model_index : gameplay.presentationModels()) {
        requirement_context = "world-model-" + std::to_string(model_index);
        const auto &scene = models[model_index].scene;
        const auto bank = scene.textureBank();
        if (bank >= 2U) {
          throw core::Error{core::ErrorCode::unsupported,
                            "Unsupported EMD texture bank"};
        }
        require_emd_scene(scene, bank);
      }
      const auto require_object_geometry =
          [&](const game::ObjectGeometry &geometry, int selected_bank) {
            if (const auto *fire =
                    std::get_if<game::ObjectFireEmitter>(&geometry)) {
              required_active_fire_ = fire;
              const auto validate_source_pages = [&](const auto &frames) {
                for (const auto &frame : frames) {
                  const auto page = timTexturePage(frame);
                  if (page >= loaded_pages_.size() ||
                      physicalTexturePage(page) != page) {
                    throw core::Error{
                        core::ErrorCode::unsupported,
                        "SPFX TIM uses a reserved display texture page"};
                  }
                }
              };
              validate_source_pages(fire->frames);
              validate_source_pages(fire->fire_frames);
              validate_source_pages(fire->breath_frames);
              validate_source_pages(fire->vapor_frames);
              return;
            }
            const auto *gmd = std::get_if<assets::GmdModel>(&geometry);
            const auto *emd = std::get_if<assets::EmdScene>(&geometry);
            const auto *hmd = std::get_if<assets::HmdModel>(&geometry);
            const auto bank = selected_bank;
            if (bank >= 2U) {
              throw core::Error{core::ErrorCode::unsupported,
                                "Unsupported object texture bank"};
            }
            if (gmd != nullptr) {
              require_mask(gmd->renderableTexturePageMask(), bank);
              for (const auto &triangle : gmd->triangles()) {
                if (triangle.flags == 0U || triangle.degenerate) {
                  continue;
                }
                const auto page = triangle.texture_page & 0x1fU;
                require_clut(triangle.clut, page, bank);
              }
            } else if (hmd != nullptr) {
              require_mask(hmd->texturePageMask(), bank);
              for (const auto &triangle : hmd->triangles()) {
                const auto page = triangle.texture_page & 0x1fU;
                require_clut(triangle.clut, page, bank);
              }
            } else if (emd != nullptr) {
              require_emd_scene(*emd, bank);
            }
          };
      for (const auto object_index : gameplay.activeObjects()) {
        requirement_context =
            "object-" + std::to_string(object_index) + "-geometry";
        const auto *object_model = gameplay.displayedObjectModel(object_index);
        if (object_model != nullptr) {
          const auto bank = gameplay.displayedObjectTextureBank(object_index);
          if (object_index < gameplay.objects().size()) {
            const auto class_id = gameplay.objects()[object_index].class_id;
            const auto weapon_crate = object_model->name == "WEPCRATE.GMD" ||
                                      object_model->name == "WEPCRATX.GMD";
            if ((class_id == 0x4fU || class_id == 0x50U) && weapon_crate &&
                bank < 2U) {
              required_crate_banks_[bank] = true;
            }
          }
          const auto *hmd_model =
              std::get_if<assets::HmdModel>(&object_model->geometry);
          if (hmd_model != nullptr &&
              !gameplay.legacyDedicatedActorPresentation(object_index) &&
              !game::legacyHmdRenderAllowed(
                  gameplay.legacyRenderCommandsAuthoritative(),
                  object_index < gameplay.objects().size() &&
                      hasLegacyHmdBones(*hmd_model,
                                        gameplay.objects()[object_index]))) {
            continue;
          }
          require_object_geometry(object_model->geometry, bank);
          const auto *state = gameplay.npcState(object_index);
          auto actor_weapon =
              hmd_model != nullptr
                  ? gameplay.legacyDedicatedActorWeapon(object_index)
                  : std::optional<game::WeaponId>{};
          if (!actor_weapon && hmd_model != nullptr && state != nullptr &&
              state->health != 0U) {
            actor_weapon = state->weapon;
          }
          if (actor_weapon) {
            if (const auto *weapon = gameplay.weaponModel(*actor_weapon)) {
              requirement_context =
                  "object-" + std::to_string(object_index) + "-weapon-" +
                  std::to_string(static_cast<unsigned int>(*actor_weapon));
              require_object_geometry(weapon->geometry,
                                      game::resident_weapon_texture_bank);
            }
          }
        }
      }
      requirement_context = "player-model";
      require_object_geometry(gameplay.playerModel().geometry,
                              game::resident_hmd_texture_bank);
      const auto require_player_weapon = [&](game::WeaponId weapon_id) {
        const auto *weapon = gameplay.weaponModel(weapon_id);
        if (weapon == nullptr) {
          return;
        }
        requirement_context =
            "player-weapon-" +
            std::to_string(static_cast<unsigned int>(weapon_id));
        require_object_geometry(weapon->geometry,
                                game::resident_weapon_texture_bank);
      };
      require_player_weapon(gameplay.hud().inventory().current());
      if (const auto frame = gameplay.legacyPresentationFrame();
          frame && frame->renderer &&
          frame->renderer->state.flashlight_enabled &&
          gameplay.hud().inventory().current() != game::WeaponId::flashlight) {
        // The retail vertex-light record is the authoritative held-tool
        // state. Keep FLASHLT resident even if the interpolated native HUD
        // selection trails the guest by one presentation frame.
        require_player_weapon(game::WeaponId::flashlight);
      }
      const auto needs_dynamic_fire = needsDynamicFire(gameplay);
      if (needs_dynamic_fire) {
        const auto fire = std::ranges::find_if(
            gameplay.objectModels(), [](const game::ObjectModel &model) {
              return std::holds_alternative<game::ObjectFireEmitter>(
                  model.geometry);
            });
        if (fire != gameplay.objectModels().end()) {
          requirement_context = "dynamic-fire";
          require_object_geometry(fire->geometry, object_bank);
        }
      }
      if (const auto frame = gameplay.legacyPresentationFrame();
          frame && frame->renderer) {
        const auto &scrim_state = frame->renderer->state.scrim;
        if (scrim_state.resource_present) {
          const auto *scrim = gameplay.detachedScrimModel();
          if (scrim == nullptr) {
            throw core::Error{core::ErrorCode::not_found,
                              "Resident retail SCRIM.EMD is missing"};
          }
          requirement_context = "retail-scrim";
          const auto scrim_model_bank = static_cast<int>(scrim->textureBank());
          require_emd_scene(*scrim, scrim_model_bank);
          // The thirteen retail MoveImage packets form a CPU-side texture
          // ring below. Copy-only source pages therefore consume no native
          // VRAM alias; only pages sampled by renderable geometry are kept
          // resident. Animated destinations are uploaded into their ordinary
          // scene mapping so world geometry sees the authored result.
          requirement_context = "retail-scrim-copy-destination";
          for (const auto &move : scrim_state.vram_moves) {
            const auto page =
                static_cast<unsigned int>(move.destination_x / 64) +
                static_cast<unsigned int>(move.destination_y / 256) * 16U;
            // A portal can expose TRAIN.EMD instances from both mission
            // banks. Preserve every already-required destination variant;
            // the CPU ring below publishes one coherent phase to both.
            for (auto bank = 0; bank < 2; ++bank) {
              const auto slot = required_page_remap_[bank][page];
              const auto expected_bank = texturePageSourceBank(page, bank);
              if (slot < required_pages_.size() &&
                  required_pages_[slot] == static_cast<int>(page) &&
                  required_banks_[slot] == expected_bank) {
                append_owner(slot);
                required_scrim_destination_pages_[slot] = true;
              }
            }
          }
        }
        const auto fire_model = std::ranges::find_if(
            gameplay.objectModels(), [](const game::ObjectModel &model) {
              return std::holds_alternative<game::ObjectFireEmitter>(
                  model.geometry);
            });
        const auto *fire_source =
            fire_model == gameplay.objectModels().end()
                ? nullptr
                : &std::get<game::ObjectFireEmitter>(fire_model->geometry);
        const auto require_guest_sprite = [&](const auto &sprite) {
          if ((sprite.attribute & 0x80000000U) != 0U || sprite.width == 0U ||
              sprite.height == 0U) {
            return;
          }
          if (fire_source != nullptr &&
              matchFireGuestSprite(*fire_source, sprite)) {
            required_active_fire_ = fire_source;
            return;
          }
          if (sprite.effect_family != 0U) {
            throw core::Error{
                core::ErrorCode::invalid_format,
                "Retail particle SPRITE provenance does not match SPFX"};
          }
          if (environment_atlas_.contains(sprite)) {
            requires_environment_atlas = true;
            return;
          }
          const auto source_page =
              static_cast<unsigned int>(sprite.tpage & 0x1fU);
          const auto clut = static_cast<std::uint16_t>(
              (static_cast<unsigned int>(sprite.center_y) << 6U) |
              ((static_cast<unsigned int>(sprite.center_x) >> 4U) & 0x3fU));
          requirement_context = "guest-camera-sprite";
          // FUN_800c84f4 builds this list for the retail camera, which can
          // already have crossed a texture-bank portal while currentRoom()
          // still names the previous room. Loading these resources through
          // the room bank and sampling them through the camera bank produces
          // exactly the unrelated palettes and oversized false panels seen
          // during transitions.
          require_page(source_page, guest_sprite_bank);
          require_clut(clut, source_page, guest_sprite_bank);
        };
        for (const auto &sprite : frame->renderer->state.guest_sprites) {
          require_guest_sprite(sprite);
        }
        // The guest runs at 20 Hz while presentation can run at 60 Hz. Glass
        // fragments may be born and expire between two displayed guest
        // snapshots, so their edge queue deliberately retains them for one
        // host frame. Keep the matching VLF page/CLUT resident for that same
        // frame; otherwise the packet survives but samples an evicted alias.
        for (const auto &entry : retained_sprites) {
          require_guest_sprite(entry.sprite);
        }
        for (const auto &shard : glass_shards) {
          const auto source_page =
              static_cast<unsigned int>(shard.texture_page & 0x1fU);
          requirement_context = "glass-shard";
          require_page(source_page, shard.texture_bank);
          require_clut(shard.clut, shard.texture_page, shard.texture_bank);
        }
      }
      if (requires_environment_atlas) {
        requirement_context = "retail-weather";
        const auto available =
            allocate_free_page(environment_page_marker, environment_bank);
        required_environment_page_ = available;
        required_pages_[available] = environment_page_marker;
        required_banks_[available] = environment_bank;
        append_owner(available);
      }
      const auto base_clut = vlfClut(vlf, page_mask_);
      required_clut_.assign(base_clut.begin(), base_clut.end());
      constexpr std::size_t row_bytes = 256U * 2U;
      std::array<bool, 32> reserved_rows{};
      std::array<bool, 32> occupied_rows{};
      for (std::size_t row = 0; row < reserved_rows.size(); ++row) {
        reserved_rows[row] =
            required_clut_sources_[0][row] || required_clut_sources_[1][row];
      }
      const auto assign_clut = [&](std::size_t logical_row, int bank,
                                   std::size_t physical_row) {
        required_clut_row_remap_[static_cast<std::size_t>(bank)][logical_row] =
            static_cast<unsigned int>(physical_row);
        const auto source =
            clut_for_bank(bank).subspan(logical_row * row_bytes, row_bytes);
        std::copy(source.begin(), source.end(),
                  required_clut_.begin() + physical_row * row_bytes);
        occupied_rows[physical_row] = true;
      };
      for (std::size_t row = 0; row < reserved_rows.size(); ++row) {
        if (!reserved_rows[row]) {
          continue;
        }
        const auto preferred_bank =
            object_bank >= 0 && object_bank < 2 &&
                    required_clut_sources_[static_cast<std::size_t>(
                        object_bank)][row]
                ? object_bank
            : required_clut_sources_[0][row] ? 0
                                             : 1;
        assign_clut(row, preferred_bank, row);

        const auto other_bank = 1 - preferred_bank;
        if (!required_clut_sources_[static_cast<std::size_t>(other_bank)]
                                   [row]) {
          continue;
        }
        const auto preferred =
            clut_for_bank(preferred_bank).subspan(row * row_bytes, row_bytes);
        const auto other =
            clut_for_bank(other_bank).subspan(row * row_bytes, row_bytes);
        if (std::equal(preferred.begin(), preferred.end(), other.begin(),
                       other.end())) {
          required_clut_row_remap_[static_cast<std::size_t>(other_bank)][row] =
              static_cast<unsigned int>(row);
          continue;
        }

        auto escape = reserved_rows.size();
        while (escape > 0U) {
          --escape;
          if (!reserved_rows[escape] && !occupied_rows[escape]) {
            break;
          }
        }
        if (reserved_rows[escape] || occupied_rows[escape]) {
          throw core::Error{core::ErrorCode::unsupported,
                            "Active scene exhausts the VRAM CLUT alias pool"};
        }
        assign_clut(row, other_bank, escape);
      }
      for (std::size_t bank = 0U; bank < required_crate_banks_.size(); ++bank) {
        if (!required_crate_banks_[bank]) {
          continue;
        }
        const auto physical_row = required_clut_row_remap_
            [bank][RetailCrateTextureOverlay::sourceClutRow()];
        crate_texture_overlay_.copyClut(
            std::span<std::byte>{required_clut_}.subspan(
                physical_row * row_bytes, row_bytes));
        required_crate_overlay_pages_[required_page_remap_[bank][12U]] = true;
        required_crate_overlay_pages_[required_page_remap_[bank][13U]] = true;
      }
      // A slot which leaves the active requirement set no longer has a
      // trustworthy resident owner. Native effect/framebuffer scratch may use
      // it before the same logical page is requested again. Forget that stale
      // claim now so a later re-entry always uploads the authored page.
      for (std::size_t slot = 0; slot < required_pages_.size(); ++slot) {
        if (required_pages_[slot] < 0) {
          loaded_pages_[slot] = PageState{};
          scrim_dirty_pages_[slot] = false;
          crate_overlay_pages_[slot] = false;
          runtime_expected_hash_valid_[slot] = false;
        } else if (scrim_dirty_pages_[slot] &&
                   !required_scrim_destination_pages_[slot]) {
          // The live MoveImage ring owns these bytes only while its retail
          // controller still targets this exact physical residency. Once the
          // controller disappears (or the remap changes), the next segment
          // must see the authored page rather than the last animated frame.
          loaded_pages_[slot] = PageState{};
          scrim_dirty_pages_[slot] = false;
          runtime_expected_hash_valid_[slot] = false;
        } else if (crate_overlay_pages_[slot] &&
                   !required_crate_overlay_pages_[slot]) {
          // WEPTRIM/WEPINNER/WEPTOP are native overlays, not authored TP12/13
          // contents.  Do not let an overlay survive after the last weapon
          // crate using it leaves the active object set.
          loaded_pages_[slot] = PageState{};
          crate_overlay_pages_[slot] = false;
          runtime_expected_hash_valid_[slot] = false;
        }
      }

      // LoadImage/MoveImage calls made by unrelated effects can change VRAM
      // without changing this streamer's ownership records. Validate only
      // immutable shared VLF pages required by the new scene. A SCRIM
      // destination is intentionally mutable: it is a persistent PS1 texture
      // ring shared by the train/sign geometry and must survive ordinary room
      // and object-list changes. Comparing it with the authored source here
      // used to reload it on nearly every guest frame, discard its accumulated
      // phase and splice pieces of successive segment states together.
      for (std::size_t slot = 0; slot < required_pages_.size(); ++slot) {
        const auto logical_page = required_pages_[slot];
        if (logical_page < 0 || required_banks_[slot] != shared_bank ||
            loaded_pages_[slot].logical_page != logical_page ||
            loaded_pages_[slot].bank != shared_bank) {
          continue;
        }
        readTexturePageAt(static_cast<unsigned int>(slot),
                          shared_page_readback_);
        const auto resident = std::as_bytes(std::span{shared_page_readback_});
        const auto authored =
            vlfPage(vlf, page_mask_, static_cast<unsigned int>(logical_page));
        if (texturePageNeedsAuthoredReload(resident, authored,
                                           scrim_dirty_pages_[slot])) {
          loaded_pages_[slot] = PageState{};
        }
      }
      const auto room = gameplay.currentRoom();
      if (textureDiagnosticsEnabled() &&
          (required_pages_ != logged_required_pages_ ||
           required_banks_ != logged_required_banks_ || room != logged_room_ ||
           object_bank != logged_object_bank_ ||
           guest_sprite_bank != logged_guest_sprite_bank_)) {
        ++texture_diagnostic_sequence_;
        const auto frame = gameplay.legacyPresentationFrame();
        PsyX_Log_Info(
            "[TextureDiag][residency] sequence=%llu guest_frame=%llu room=%zu "
            "object_bank=%d camera_bank=%d active_models=%zu "
            "active_objects=%zu revision=%llu\n",
            static_cast<unsigned long long>(texture_diagnostic_sequence_),
            static_cast<unsigned long long>(frame ? frame->guest_frame : 0U),
            room, object_bank, guest_sprite_bank,
            gameplay.presentationModels().size(),
            gameplay.activeObjects().size(),
            static_cast<unsigned long long>(residency_revision_));
        for (std::size_t slot = 0; slot < required_pages_.size(); ++slot) {
          if (required_pages_[slot] < 0) {
            continue;
          }
          auto owners = std::string{};
          for (const auto &owner : required_page_owners_[slot]) {
            if (!owners.empty()) {
              owners += '|';
            }
            owners += owner;
          }
          PsyX_Log_Info(
              "[TextureDiag][resident-slot] sequence=%llu physical=%zu "
              "logical=%d bank=%d loaded=%d/%d generation=%llu owners=%s\n",
              static_cast<unsigned long long>(texture_diagnostic_sequence_),
              slot, required_pages_[slot], required_banks_[slot],
              loaded_pages_[slot].logical_page, loaded_pages_[slot].bank,
              static_cast<unsigned long long>(page_generations_[slot]),
              owners.empty() ? "unclassified" : owners.c_str());
        }
        logged_required_pages_ = required_pages_;
        logged_required_banks_ = required_banks_;
        logged_room_ = room;
        logged_object_bank_ = object_bank;
        logged_guest_sprite_bank_ = guest_sprite_bank;
      }
      requirements_valid_ = true;
    }

    // Rendering reads this legacy lookup directly. Re-publish the cached
    // requirement result every frame so transient VRAM ownership invalidation
    // can never leave geometry sampling a stale logical-to-physical mapping.
    streamed_texture_page_remap = required_page_remap_;
    streamed_clut_row_remap = required_clut_row_remap_;

    std::array<bool, resident_texture_page_count> capture_expected_pages{};
    auto changed = false;
    for (unsigned int slot = 0; slot < required_pages_.size(); ++slot) {
      const auto page = required_pages_[slot];
      const auto bank = required_banks_[slot];
      if (page < 0 || (loaded_pages_[slot].logical_page == page &&
                       loaded_pages_[slot].bank == bank)) {
        continue;
      }
      if (bank == environment_bank) {
        environment_atlas_.upload(slot);
      } else if (bank == shared_bank) {
        uploadTexturePageAt(
            slot, vlfPage(vlf, page_mask_, static_cast<unsigned int>(page)));
      } else {
        uploadTexturePageAt(
            slot, mission_.textureBank(static_cast<std::size_t>(bank))
                      .file(texturePageName(static_cast<unsigned int>(page))));
      }
      loaded_pages_[slot] = PageState{page, bank};
      scrim_dirty_pages_[slot] = false;
      crate_overlay_pages_[slot] = false;
      runtime_expected_hash_valid_[slot] = false;
      ++page_generations_[slot];
      page_upload_write_sequences_[slot] = GR_GetVRAMWriteSequence();
      reported_mismatch_hashes_[slot] = 0U;
      ++residency_revision_;
      changed = true;
    }

    if (loaded_clut_ != required_clut_) {
      uploadClut(required_clut_);
      loaded_clut_ = required_clut_;
      changed = true;
    }
    for (std::size_t bank = 0U; bank < required_crate_banks_.size(); ++bank) {
      if (!required_crate_banks_[bank]) {
        crate_overlay_tokens_[bank] = {};
        continue;
      }
      const auto trim_page = residentPhysicalPage(12U, static_cast<int>(bank));
      const auto top_page = residentPhysicalPage(13U, static_cast<int>(bank));
      if (!trim_page || !top_page) {
        throw core::Error{core::ErrorCode::unsupported,
                          "Resident weapon-crate texture pages are missing"};
      }
      const std::array tokens{
          page_generations_[*trim_page],
          page_generations_[*top_page],
      };
      if (crate_overlay_tokens_[bank] != tokens) {
        crate_texture_overlay_.upload(*trim_page, *top_page);
        crate_overlay_pages_[*trim_page] = true;
        crate_overlay_pages_[*top_page] = true;
        capture_expected_pages[*trim_page] = true;
        capture_expected_pages[*top_page] = true;
        crate_overlay_tokens_[bank] = tokens;
        changed = true;
      }
    }
    if (active_fire_ != required_active_fire_) {
      active_fire_ = required_active_fire_;
      fire_placement_ = active_fire_ != nullptr
                            ? uploadFireTextureAtlas(*active_fire_)
                            : FireTexturePlacement{};
      changed = changed || active_fire_ != nullptr;
    }
    if (changed) {
      DrawSync(0);
    }
    for (std::size_t slot = 0; slot < crate_overlay_pages_.size(); ++slot) {
      if (capture_expected_pages[slot]) {
        captureExpectedPhysicalPage(static_cast<unsigned int>(slot));
      }
    }
    if (textureDiagnosticsEnabled()) {
      diagnoseResidentPage(gameplay, vlf);
    }
  }

  void invalidate() noexcept {
    // Only resident VRAM ownership is lost here. Source requirements remain
    // valid until requirementsChanged observes an exact gameplay input change.
    // Force one exact rescan after a reset/room reload even when the guest
    // presentation sequence happens to restart at the same numeric value.
    requirement_presentation_sequence_ =
        std::numeric_limits<std::uint64_t>::max();
    transient_requirement_inputs_.clear();
    loaded_pages_.fill(PageState{});
    scrim_dirty_pages_.fill(false);
    crate_overlay_pages_.fill(false);
    runtime_expected_hash_valid_.fill(false);
    loaded_clut_.clear();
    fire_placement_ = {};
    active_fire_ = nullptr;
    crate_overlay_tokens_ = {};
  }

  void invalidatePhysicalPage(unsigned int physical_page) noexcept {
    if (physical_page < loaded_pages_.size()) {
      loaded_pages_[physical_page] = PageState{};
      scrim_dirty_pages_[physical_page] = false;
      crate_overlay_pages_[physical_page] = false;
      runtime_expected_hash_valid_[physical_page] = false;
    }
  }

  [[nodiscard]] std::optional<unsigned int>
  residentPhysicalPage(unsigned int logical_page,
                       int texture_bank) const noexcept {
    if (!requirements_valid_ || logical_page >= 32U || texture_bank < 0 ||
        texture_bank >= 2) {
      return std::nullopt;
    }
    const auto bank = static_cast<std::size_t>(texture_bank);
    const auto physical_page = required_page_remap_[bank][logical_page];
    if (physical_page >= required_pages_.size()) {
      return std::nullopt;
    }
    const auto expected_bank =
        texturePageSourceBank(logical_page, texture_bank);
    if (required_pages_[physical_page] != static_cast<int>(logical_page) ||
        required_banks_[physical_page] != expected_bank ||
        loaded_pages_[physical_page].logical_page !=
            static_cast<int>(logical_page) ||
        loaded_pages_[physical_page].bank != expected_bank) {
      return std::nullopt;
    }
    return physical_page;
  }

  [[nodiscard]] std::optional<std::uint64_t>
  residentPageToken(unsigned int logical_page,
                    int texture_bank) const noexcept {
    const auto physical_page = residentPhysicalPage(logical_page, texture_bank);
    if (!physical_page) {
      return std::nullopt;
    }
    return residentTexturePageToken(page_generations_[*physical_page],
                                    *physical_page);
  }

  [[nodiscard]] std::uint64_t residencyRevision() const noexcept {
    return residency_revision_;
  }


  [[nodiscard]] std::uint8_t
  residentTextureBankMask(unsigned int logical_page) const noexcept {
    if (logical_page >= 32U) {
      return 0U;
    }
    auto mask = std::uint8_t{};
    for (auto bank = 0; bank < 2; ++bank) {
      if (residentPhysicalPage(logical_page, bank)) {
        mask = static_cast<std::uint8_t>(mask | (1U << bank));
      }
    }
    return mask;
  }

  [[nodiscard]] std::span<const std::byte>
  authoredTexturePage(unsigned int logical_page, int texture_bank) const {
    if (logical_page >= 32U || texture_bank < 0 || texture_bank >= 2) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Authored texture-page request is invalid"};
    }
    const auto source_bank = texturePageSourceBank(logical_page, texture_bank);
    if (source_bank == shared_bank) {
      const auto vlf = mission_.archive().file("VLF.RFF");
      return vlfPage(vlf, page_mask_, logical_page);
    }
    return mission_.textureBank(static_cast<std::size_t>(source_bank))
        .file(texturePageName(logical_page));
  }

  [[nodiscard]] bool
  uploadRuntimeTexturePage(unsigned int logical_page, int texture_bank,
                           std::span<const std::byte> bytes) {
    if (bytes.size() != texture_page_bytes) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Runtime texture page has an invalid size"};
    }
    const auto physical_page = residentPhysicalPage(logical_page, texture_bank);
    if (!physical_page) {
      return false;
    }
    uploadTexturePageAt(*physical_page, bytes);
    scrim_dirty_pages_[*physical_page] = true;
    crate_overlay_pages_[*physical_page] = false;
    runtime_expected_hash_[*physical_page] = fingerprint(bytes);
    runtime_expected_hash_valid_[*physical_page] = true;
    page_upload_write_sequences_[*physical_page] = GR_GetVRAMWriteSequence();
    reported_mismatch_hashes_[*physical_page] = 0U;
    return true;
  }

  [[nodiscard]] bool reloadResidentPage(unsigned int logical_page,
                                        int texture_bank) {
    const auto physical_page = residentPhysicalPage(logical_page, texture_bank);
    if (!physical_page) {
      return false;
    }
    uploadTexturePageAt(*physical_page,
                        authoredTexturePage(logical_page, texture_bank));
    ++page_generations_[*physical_page];
    scrim_dirty_pages_[*physical_page] = false;
    crate_overlay_pages_[*physical_page] = false;
    runtime_expected_hash_valid_[*physical_page] = false;
    page_upload_write_sequences_[*physical_page] = GR_GetVRAMWriteSequence();
    reported_mismatch_hashes_[*physical_page] = 0U;
    ++residency_revision_;
    return true;
  }

  void captureExpectedPhysicalPage(unsigned int physical_page) {
    if (physical_page >= runtime_expected_hash_valid_.size()) {
      return;
    }
    readTexturePageAt(physical_page, shared_page_readback_);
    runtime_expected_hash_[physical_page] =
        fingerprint(std::as_bytes(std::span{shared_page_readback_}));
    runtime_expected_hash_valid_[physical_page] = true;
  }

  [[nodiscard]] std::optional<game::EffectTextureCopy>
  relocateEffectTextureCopy(const game::EffectTextureCopy &copy,
                            int texture_bank) const noexcept {
    if (!requirements_valid_ || texture_bank < 0 || texture_bank >= 2) {
      return std::nullopt;
    }
    const auto relocate =
        [&](int x, int y, int width,
            int height) -> std::optional<game::EffectVramRect> {
      if (x < 0 || y < 0 || width <= 0 || height <= 0 || x >= 1024 ||
          y >= 512) {
        return std::nullopt;
      }
      const auto logical_page = static_cast<unsigned int>(x / 64) +
                                (static_cast<unsigned int>(y / 256) * 16U);
      const auto local_x = x & 63;
      const auto local_y = y & 255;
      if (local_x + width > 64 || local_y + height > 256) {
        return std::nullopt;
      }
      const auto physical_page =
          residentPhysicalPage(logical_page, texture_bank);
      if (!physical_page || *physical_page >= psx_texture_page_count) {
        return std::nullopt;
      }
      return game::EffectVramRect{
          static_cast<std::int16_t>((*physical_page & 15U) * 64U +
                                    static_cast<unsigned int>(local_x)),
          static_cast<std::int16_t>((*physical_page > 15U ? 256U : 0U) +
                                    static_cast<unsigned int>(local_y)),
          static_cast<std::int16_t>(width),
          static_cast<std::int16_t>(height),
      };
    };

    const auto source = relocate(copy.source.x, copy.source.y,
                                 copy.source.width, copy.source.height);
    const auto destination = relocate(copy.destination_x, copy.destination_y,
                                      copy.source.width, copy.source.height);
    if (!source || !destination) {
      return std::nullopt;
    }
    return game::EffectTextureCopy{
        *source,
        destination->x,
        destination->y,
    };
  }

  [[nodiscard]] const FireTexturePlacement &firePlacement() const noexcept {
    return fire_placement_;
  }

  [[nodiscard]] std::optional<GuestSpriteTexturePlacement> relocateGuestSprite(
      const game::LegacyGuestSpriteBridgeState &sprite) const noexcept {
    if (active_fire_ != nullptr) {
      if (const auto match = matchFireGuestSprite(*active_fire_, sprite)) {
        if (const auto *placement =
                fire_placement_.frame(match->family, match->frame)) {
          return GuestSpriteTexturePlacement{
              placement->texture_page,
              placement->clut,
              static_cast<std::uint8_t>(
                  placement->minimum_u +
                  (static_cast<unsigned int>(sprite.u) - match->source_u)),
              static_cast<std::uint8_t>(
                  placement->minimum_v +
                  (static_cast<unsigned int>(sprite.v) - match->source_v)),
          };
        }
      }
    }
    if (required_environment_page_) {
      if (const auto environment = environment_atlas_.relocate(
              sprite, *required_environment_page_)) {
        return environment;
      }
    }
    return std::nullopt;
  }

private:
  struct PageState {
    int logical_page{unused_bank};
    int bank{unused_bank};
  };

  static constexpr int shared_bank = -2;
  static constexpr int unused_bank = -3;
  static constexpr int environment_bank = -4;
  static constexpr int environment_page_marker = 32;

  [[nodiscard]] int texturePageSourceBank(unsigned int logical_page,
                                          int texture_bank) const noexcept {
    texture_bank =
        canonicalMissionTextureBank(texture_bank, mission_.textureBankCount());
    // SCRIM destinations are mutable live VRAM, not immutable authored
    // pages. Bank-zero and bank-one copies may start equal yet diverge after
    // the first DR_MOVE, so they must never share one physical alias.
    if (logical_page < 32U &&
        (scrim_destination_page_mask_ & (1U << logical_page)) != 0U) {
      return texture_bank;
    }
    if (logical_page < 32U && texture_bank >= 0 && texture_bank < 2 &&
        vlf_compatible_pages_[static_cast<std::size_t>(texture_bank)]
                             [logical_page]) {
      return shared_bank;
    }
    // VLF compatibility and bank compatibility are separate questions.
    // Two authored banks may both differ from the initial VLF page while
    // remaining byte-identical to each other. Treating those copies as two
    // resident resources needlessly exhausts the native 26-page alias pool.
    // Exact byte equality makes bank zero a safe canonical source; palette
    // rows remain independently selected and deduplicated by require_clut.
    if (logical_page < 32U && texture_bank == 1 &&
        texture_banks_equivalent_[logical_page]) {
      return 0;
    }
    return texture_bank;
  }

  enum class RequirementInputKind : std::uint8_t {
    room,
    object_bank,
    guest_sprite_bank,
    active_model,
    active_object,
    object_texture_bank,
    displayed_model,
    object_render_eligible,
    npc_weapon,
    dedicated_actor_weapon,
    player_model,
    player_texture_bank,
    player_weapon,
    dynamic_fire,
    guest_sprite_material,
    guest_sprite_rectangle,
    guest_sprite_effect,
    guest_sprite_state,
    glass_shard_material,
    retail_scrim,
    retail_scrim_model,
    retail_scrim_bank,
    retail_scrim_move,
    retail_scrim_move_source,
    retail_scrim_move_destination,
    retail_scrim_move_extent,
  };

  [[nodiscard]] static std::uint64_t
  fingerprint(std::span<const std::byte> bytes) noexcept {
    auto value = std::uint64_t{1469598103934665603ULL};
    for (const auto byte : bytes) {
      value ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte));
      value *= 1099511628211ULL;
    }
    return value;
  }

  [[nodiscard]] static const char *vramWriteKindName(int kind) noexcept {
    switch (kind) {
    case GR_VRAM_WRITE_UPLOAD:
      return "upload";
    case GR_VRAM_WRITE_MOVE:
      return "move";
    case GR_VRAM_WRITE_CLEAR:
      return "clear";
    case GR_VRAM_WRITE_FRAMEBUFFER:
      return "framebuffer";
    default:
      return "unknown";
    }
  }

  void diagnoseResidentPage(const game::GameplaySession &gameplay,
                            std::span<const std::byte> vlf) {
    const auto frame = gameplay.legacyPresentationFrame();
    if (!frame || frame->guest_frame == last_diagnostic_guest_frame_) {
      return;
    }
    last_diagnostic_guest_frame_ = frame->guest_frame;

    for (auto attempt = std::size_t{}; attempt < required_pages_.size();
         ++attempt) {
      const auto slot = diagnostic_page_cursor_++ % required_pages_.size();
      const auto logical_page = required_pages_[slot];
      const auto bank = required_banks_[slot];
      if (logical_page < 0 || logical_page >= 32 ||
          loaded_pages_[slot].logical_page != logical_page ||
          loaded_pages_[slot].bank != bank || bank == environment_bank) {
        continue;
      }

      const auto expected =
          bank == shared_bank
              ? vlfPage(vlf, page_mask_,
                        static_cast<unsigned int>(logical_page))
              : mission_.textureBank(static_cast<std::size_t>(bank))
                    .file(texturePageName(
                        static_cast<unsigned int>(logical_page)));
      readTexturePageAt(static_cast<unsigned int>(slot), shared_page_readback_);
      const auto resident = std::as_bytes(std::span{shared_page_readback_});
      if (std::equal(resident.begin(), resident.end(), expected.begin(),
                     expected.end())) {
        reported_mismatch_hashes_[slot] = 0U;
        break;
      }

      const auto actual_hash = fingerprint(resident);
      if (runtime_expected_hash_valid_[slot] &&
          runtime_expected_hash_[slot] == actual_hash) {
        reported_mismatch_hashes_[slot] = 0U;
        break;
      }
      if (reported_mismatch_hashes_[slot] == actual_hash) {
        break;
      }
      reported_mismatch_hashes_[slot] = actual_hash;
      auto first_difference = std::size_t{};
      while (first_difference < expected.size() &&
             resident[first_difference] == expected[first_difference]) {
        ++first_difference;
      }
      auto owners = std::string{};
      for (const auto &owner : required_page_owners_[slot]) {
        if (!owners.empty()) {
          owners += '|';
        }
        owners += owner;
      }
      const auto baseline = page_upload_write_sequences_[slot];
      std::array<GrVRAMWriteEvent, 512U> writes{};
      const auto write_count = GR_ReadVRAMWriteEvents(
          baseline, writes.data(), static_cast<int>(writes.size()));
      const auto page_x = static_cast<int>((slot & 15U) * 64U);
      const auto page_y = slot > 15U ? 256 : 0;
      const auto intersects_page = [&](const GrVRAMWriteEvent &write) {
        return slot < psx_texture_page_count &&
               write.destination_x < page_x + 64 &&
               write.destination_x + write.width > page_x &&
               write.destination_y < page_y + 256 &&
               write.destination_y + write.height > page_y;
      };
      auto overlapping_writes = std::size_t{};
      for (auto index = 0; index < write_count; ++index) {
        overlapping_writes += intersects_page(writes[index]) ? 1U : 0U;
      }
      ++texture_diagnostic_sequence_;
      PsyX_Log_Warning(
          "[TextureDiag][mismatch] sequence=%llu guest_frame=%llu room=%zu "
          "physical=%zu logical=%d bank=%d generation=%llu "
          "expected_hash=%016llx actual_hash=%016llx first_byte=%zu "
          "upload_write=%llu current_write=%llu journal_lost=%u "
          "overlapping_writes=%zu owners=%s\n",
          static_cast<unsigned long long>(texture_diagnostic_sequence_),
          static_cast<unsigned long long>(frame->guest_frame),
          gameplay.currentRoom(), slot, logical_page, bank,
          static_cast<unsigned long long>(page_generations_[slot]),
          static_cast<unsigned long long>(fingerprint(expected)),
          static_cast<unsigned long long>(actual_hash), first_difference,
          static_cast<unsigned long long>(baseline), GR_GetVRAMWriteSequence(),
          write_count > 0 && writes[0].sequence > baseline + 1U ? 1U : 0U,
          overlapping_writes, owners.empty() ? "unclassified" : owners.c_str());
      for (auto index = 0; index < write_count; ++index) {
        const auto &write = writes[index];
        if (!intersects_page(write)) {
          continue;
        }
        PsyX_Log_Warning(
            "[TextureDiag][write] mismatch_sequence=%llu write=%llu kind=%s "
            "source=(%d,%d) destination=(%d,%d) size=%dx%d\n",
            static_cast<unsigned long long>(texture_diagnostic_sequence_),
            write.sequence, vramWriteKindName(write.kind), write.source_x,
            write.source_y, write.destination_x, write.destination_y,
            write.width, write.height);
      }
      break;
    }
  }

  struct RequirementInput {
    RequirementInputKind kind{};
    std::uintptr_t value{};

    friend bool operator==(const RequirementInput &,
                           const RequirementInput &) = default;
  };

  [[nodiscard]] static bool
  needsDynamicFire(const game::GameplaySession &gameplay) noexcept {
    return !gameplay.legacyExplParticles().empty() ||
           !gameplay.legacyPark2FlamethrowerRibbons().empty() ||
           gameplay.hud().inventory().current() ==
               game::WeaponId::flamethrower ||
           std::ranges::any_of(gameplay.projectiles(),
                               [](const game::GameplayProjectile &projectile) {
                                 return projectile.active &&
                                        projectile.phase ==
                                            game::ProjectilePhase::explosion;
                               }) ||
           std::ranges::any_of(
               gameplay.effects(), [](const game::GameplayEffect &effect) {
                 return effect.type == game::GameplayEffectType::explosion ||
                        effect.type == game::GameplayEffectType::burning_fire;
               });
  }

  [[nodiscard]] bool requirementsChanged(
      const game::GameplaySession &gameplay, int object_bank,
      int guest_sprite_bank,
      std::span<const game::LegacyGuestSpritePresentationState>
          retained_sprites,
      std::span<const GlassShardPresentationState> glass_shards) {
    scratch_transient_requirement_inputs_.clear();
    const auto append_transient = [this](RequirementInputKind kind,
                                         std::uintptr_t value) {
      scratch_transient_requirement_inputs_.push_back(
          RequirementInput{kind, value});
    };
    // These scalar selectors are cheap and also protect out-of-band UI or
    // reset transitions which may alter presentation state before the next
    // 20 Hz guest sequence is published.
    append_transient(RequirementInputKind::room, gameplay.currentRoom());
    append_transient(RequirementInputKind::object_bank,
                     static_cast<std::uintptr_t>(object_bank));
    append_transient(RequirementInputKind::guest_sprite_bank,
                     static_cast<std::uintptr_t>(guest_sprite_bank));
    append_transient(RequirementInputKind::active_model,
                     gameplay.presentationModels().size());
    append_transient(RequirementInputKind::active_object,
                     gameplay.activeObjects().size());
    append_transient(
        RequirementInputKind::player_weapon,
        static_cast<std::uintptr_t>(gameplay.hud().inventory().current()));
    for (const auto &shard : glass_shards) {
      const auto material =
          static_cast<std::uintptr_t>(shard.texture_page) |
          (static_cast<std::uintptr_t>(shard.clut) << 16U) |
          (static_cast<std::uintptr_t>(shard.texture_bank) << 32U);
      append_transient(RequirementInputKind::glass_shard_material, material);
    }
    const auto append_transient_sprite = [&](const auto &sprite) {
      append_transient(
          RequirementInputKind::guest_sprite_material,
          static_cast<std::uintptr_t>(sprite.tpage) |
              (static_cast<std::uintptr_t>(sprite.center_x) << 16U) |
              (static_cast<std::uintptr_t>(sprite.center_y) << 32U));
      append_transient(RequirementInputKind::guest_sprite_rectangle,
                       static_cast<std::uintptr_t>(sprite.u) |
                           (static_cast<std::uintptr_t>(sprite.v) << 8U) |
                           (static_cast<std::uintptr_t>(sprite.width) << 16U) |
                           (static_cast<std::uintptr_t>(sprite.height) << 32U));
      append_transient(
          RequirementInputKind::guest_sprite_effect,
          static_cast<std::uintptr_t>(sprite.effect_family) |
              (static_cast<std::uintptr_t>(sprite.effect_frame) << 8U));
      append_transient(RequirementInputKind::guest_sprite_state,
                       (sprite.attribute & 0x80000000U) != 0U ? 1U : 0U);
    };
    for (const auto &entry : retained_sprites) {
      append_transient_sprite(entry.sprite);
    }
    const auto presentation_sequence = gameplay.legacyPresentationSequence();
    // A legacy presentation sequence is the atomic 20 Hz publication of all
    // room/model/object/resource selectors below. Host-retained sprites and
    // glass are the only resource users which can change between those guest
    // publications, so compare their exact selector tuples independently.
    // This keeps 240 Hz presentation from rescanning the complete scene while
    // preserving the full exact-key rebuild at every guest state transition.
    if (requirements_valid_ &&
        requirement_presentation_sequence_ == presentation_sequence &&
        scratch_transient_requirement_inputs_ ==
            transient_requirement_inputs_) {
      return false;
    }

    scratch_requirement_inputs_.clear();
    const auto append = [this](RequirementInputKind kind,
                               std::uintptr_t value) {
      scratch_requirement_inputs_.push_back(RequirementInput{kind, value});
    };
    const auto address = [](const void *value) noexcept {
      return reinterpret_cast<std::uintptr_t>(value);
    };

    append(RequirementInputKind::room, gameplay.currentRoom());
    append(RequirementInputKind::object_bank,
           static_cast<std::uintptr_t>(object_bank));
    append(RequirementInputKind::guest_sprite_bank,
           static_cast<std::uintptr_t>(guest_sprite_bank));
    for (const auto &shard : glass_shards) {
      const auto material =
          static_cast<std::uintptr_t>(shard.texture_page) |
          (static_cast<std::uintptr_t>(shard.clut) << 16U) |
          (static_cast<std::uintptr_t>(shard.texture_bank) << 32U);
      append(RequirementInputKind::glass_shard_material, material);
    }
    for (const auto model : gameplay.presentationModels()) {
      append(RequirementInputKind::active_model, model);
    }
    for (const auto object : gameplay.activeObjects()) {
      append(RequirementInputKind::active_object, object);
      append(RequirementInputKind::object_texture_bank,
             gameplay.displayedObjectTextureBank(object));
      append(RequirementInputKind::displayed_model,
             address(gameplay.displayedObjectModel(object)));
      const auto *displayed = gameplay.displayedObjectModel(object);
      const auto *hmd =
          displayed != nullptr
              ? std::get_if<assets::HmdModel>(&displayed->geometry)
              : nullptr;
      append(RequirementInputKind::object_render_eligible,
             (hmd == nullptr ||
              gameplay.legacyDedicatedActorPresentation(object) ||
              game::legacyHmdRenderAllowed(
                  gameplay.legacyRenderCommandsAuthoritative(),
                  object < gameplay.objects().size() &&
                      hasLegacyHmdBones(*hmd, gameplay.objects()[object])))
                 ? 1U
                 : 0U);
      if (const auto *npc = gameplay.npcState(object)) {
        append(RequirementInputKind::npc_weapon,
               static_cast<std::uintptr_t>(npc->weapon));
      }
      if (const auto weapon = gameplay.legacyDedicatedActorWeapon(object)) {
        append(RequirementInputKind::dedicated_actor_weapon,
               static_cast<std::uintptr_t>(*weapon));
      }
    }
    append(RequirementInputKind::player_model,
           address(&gameplay.playerModel()));
    append(RequirementInputKind::player_texture_bank,
           game::resident_hmd_texture_bank);
    append(RequirementInputKind::player_weapon,
           static_cast<std::uintptr_t>(gameplay.hud().inventory().current()));
    append(RequirementInputKind::dynamic_fire,
           needsDynamicFire(gameplay) ? 1U : 0U);
    if (const auto frame = gameplay.legacyPresentationFrame();
        frame && frame->renderer) {
      const auto &scrim = frame->renderer->state.scrim;
      append(RequirementInputKind::retail_scrim,
             (scrim.resource_present ? 1U : 0U) | (scrim.visible ? 2U : 0U));
      const auto *scrim_model = gameplay.detachedScrimModel();
      append(RequirementInputKind::retail_scrim_model, address(scrim_model));
      append(RequirementInputKind::retail_scrim_bank,
             scrim_model != nullptr ? scrim_model->textureBank() : 0U);
      for (const auto &move : scrim.vram_moves) {
        const auto source_page =
            static_cast<std::uintptr_t>(move.source_x / 64) +
            static_cast<std::uintptr_t>(move.source_y / 256) * 16U;
        const auto destination_page =
            static_cast<std::uintptr_t>(move.destination_x / 64) +
            static_cast<std::uintptr_t>(move.destination_y / 256) * 16U;
        append(RequirementInputKind::retail_scrim_move,
               source_page | (destination_page << 5U));
        append(RequirementInputKind::retail_scrim_move_source,
               static_cast<std::uintptr_t>(move.source_x) |
                   (static_cast<std::uintptr_t>(move.source_y) << 16U));
        append(RequirementInputKind::retail_scrim_move_destination,
               static_cast<std::uintptr_t>(move.destination_x) |
                   (static_cast<std::uintptr_t>(move.destination_y) << 16U));
        append(RequirementInputKind::retail_scrim_move_extent,
               static_cast<std::uintptr_t>(move.width) |
                   (static_cast<std::uintptr_t>(move.height) << 16U));
      }
      const auto append_guest_sprite = [&](const auto &sprite) {
        append(RequirementInputKind::guest_sprite_material,
               static_cast<std::uintptr_t>(sprite.tpage) |
                   (static_cast<std::uintptr_t>(sprite.center_x) << 16U) |
                   (static_cast<std::uintptr_t>(sprite.center_y) << 32U));
        append(RequirementInputKind::guest_sprite_rectangle,
               static_cast<std::uintptr_t>(sprite.u) |
                   (static_cast<std::uintptr_t>(sprite.v) << 8U) |
                   (static_cast<std::uintptr_t>(sprite.width) << 16U) |
                   (static_cast<std::uintptr_t>(sprite.height) << 32U));
        append(RequirementInputKind::guest_sprite_effect,
               static_cast<std::uintptr_t>(sprite.effect_family) |
                   (static_cast<std::uintptr_t>(sprite.effect_frame) << 8U));
        append(RequirementInputKind::guest_sprite_state,
               (sprite.attribute & 0x80000000U) != 0U ? 1U : 0U);
      };
      for (const auto &sprite : frame->renderer->state.guest_sprites) {
        append_guest_sprite(sprite);
      }
      for (const auto &entry : retained_sprites) {
        append_guest_sprite(entry.sprite);
      }
    }

    // Exact source identity, not a hash, is the correctness boundary. This
    // mirrors every resource-selecting branch in ensure: room visibility,
    // displayed object/model identity, every actor/player/projectile weapon,
    // the dynamic-fire need and every field which selects a guest-sprite TIM.
    // Camera/animation and reset-to-an-equivalent state safely reuse the key.
    const auto requirements_match =
        requirements_valid_ &&
        scratch_requirement_inputs_ == requirement_inputs_;
    requirement_presentation_sequence_ = presentation_sequence;
    transient_requirement_inputs_.swap(scratch_transient_requirement_inputs_);
    if (requirements_match) {
      return false;
    }
    requirement_inputs_.swap(scratch_requirement_inputs_);
    return true;
  }

  const game::MissionPackage &mission_;
  EnvironmentTextureAtlas environment_atlas_;
  RetailCrateTextureOverlay crate_texture_overlay_;
  std::uint32_t page_mask_{};
  std::array<int, resident_texture_page_count> required_pages_{};
  std::array<std::array<bool, 32U>, 2U> vlf_compatible_pages_{};
  std::array<bool, 32U> texture_banks_equivalent_{};
  std::array<int, resident_texture_page_count> required_banks_{};
  std::array<std::array<bool, 32>, 2> required_clut_sources_{};
  std::array<std::array<unsigned int, 32>, 2> required_page_remap_{};
  std::array<std::array<unsigned int, 32>, 2> required_clut_row_remap_{};
  std::array<std::vector<std::string>, resident_texture_page_count>
      required_page_owners_{};
  std::vector<std::byte> required_clut_;
  std::vector<RequirementInput> requirement_inputs_;
  std::vector<RequirementInput> scratch_requirement_inputs_;
  std::vector<RequirementInput> transient_requirement_inputs_;
  std::vector<RequirementInput> scratch_transient_requirement_inputs_;
  std::uint64_t requirement_presentation_sequence_{
      std::numeric_limits<std::uint64_t>::max()};
  const game::ObjectFireEmitter *required_active_fire_{};
  std::optional<unsigned int> required_environment_page_;
  std::array<bool, 2U> required_crate_banks_{};
  std::array<bool, resident_texture_page_count> required_crate_overlay_pages_{};
  std::array<bool, resident_texture_page_count>
      required_scrim_destination_pages_{};
  std::uint32_t scrim_destination_page_mask_{};
  bool requirements_valid_{};
  std::array<PageState, resident_texture_page_count> loaded_pages_{};
  std::array<bool, resident_texture_page_count> scrim_dirty_pages_{};
  std::array<bool, resident_texture_page_count> crate_overlay_pages_{};
  std::array<std::uint64_t, resident_texture_page_count>
      runtime_expected_hash_{};
  std::array<bool, resident_texture_page_count> runtime_expected_hash_valid_{};
  std::array<std::uint64_t, resident_texture_page_count> page_generations_{};
  std::array<std::uint64_t, resident_texture_page_count>
      page_upload_write_sequences_{};
  mutable std::array<std::array<std::uint64_t, 32U>, 2U> authored_page_hash_{};
  mutable std::array<std::array<bool, 32U>, 2U> authored_page_hash_valid_{};
  std::array<std::uint64_t, resident_texture_page_count>
      reported_mismatch_hashes_{};
  std::array<int, resident_texture_page_count> logged_required_pages_{};
  std::array<int, resident_texture_page_count> logged_required_banks_{};
  std::size_t logged_room_{std::numeric_limits<std::size_t>::max()};
  int logged_object_bank_{std::numeric_limits<int>::min()};
  int logged_guest_sprite_bank_{std::numeric_limits<int>::min()};
  std::uint64_t last_diagnostic_guest_frame_{
      std::numeric_limits<std::uint64_t>::max()};
  std::size_t diagnostic_page_cursor_{};
  std::uint64_t texture_diagnostic_sequence_{};
  std::uint64_t residency_revision_{};
  std::array<unsigned short, texture_page_bytes / sizeof(unsigned short)>
      shared_page_readback_{};
  std::vector<std::byte> loaded_clut_;
  FireTexturePlacement fire_placement_{};
  const game::ObjectFireEmitter *active_fire_{};
  std::array<std::array<std::uint64_t, 2U>, 2U> crate_overlay_tokens_{};
};

struct HudTextureAsset {
  std::string name;
  assets::TimImage image;
};

[[nodiscard]] bool isNativeHdFontSheet(std::string_view name,
                                       const assets::TimImage &image) noexcept {
  if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
      image.pixels().y != 0U) {
    return false;
  }
  if (name == "FONTA.TIM") {
    return image.pixels().x == 832U && image.displayWidth() == 64U &&
           image.displayHeight() == 128U;
  }
  if (name == "FONTB.TIM") {
    return image.pixels().x == 864U && image.displayWidth() == 64U &&
           image.displayHeight() == 128U;
  }
  if (name == "FONTC.TIM") {
    return image.pixels().x == 896U && image.displayWidth() == 84U &&
           image.displayHeight() == 246U;
  }
  return false;
}

class HudTextureAtlas final {
public:
  explicit HudTextureAtlas(const game::MissionPackage &mission) {
    std::vector<HudTextureAsset> retail_fonts;
    const auto required = [](std::string_view name) {
      // The three font sheets and SYMBOL are shared by the original
      // gameplay and pause interfaces. Keep them in the same restored
      // VRAM atlas as the weapon silhouettes.
      if (name == "FONTA.TIM" || name == "FONTB.TIM" || name == "FONTC.TIM" ||
          name == "SYMBOL.TIM" || name == "SCOPED.TIM" || name == "SCP0.TIM" ||
          name == "SCP45.TIM" || name == "SCP90.TIM" || name == "SCP135.TIM" ||
          name == "SCP180.TIM" || name == "SCP225.TIM" ||
          name == "SCP270.TIM" || name == "SCP315.TIM" ||
          name == "DANGER.TIM" || name == "TARGET.TIM" || name == "ARMOR.TIM") {
        return true;
      }
      if (std::ranges::find(nightvision_scope_layers, name) !=
          nightvision_scope_layers.end()) {
        return true;
      }
      for (std::size_t index = 0; index < game::weapon_slot_count; ++index) {
        for (const auto layer :
             game::droppedItemIconLayers(static_cast<std::uint16_t>(index))) {
          if (name == layer) {
            return true;
          }
        }
      }
      return false;
    };
    for (const auto &entry : mission.interfaceAssets().entries()) {
      if (!required(entry.name)) {
        continue;
      }
      const auto font_asset = entry.name == "FONTA.TIM" ||
                              entry.name == "FONTB.TIM" ||
                              entry.name == "FONTC.TIM";
      auto localized = font_asset
                           ? game::readLocalizedAsset("fonts/" + entry.name)
                           : std::nullopt;
      if (font_asset && game::russianLanguageActive()) {
        retail_fonts.push_back(HudTextureAsset{
            entry.name,
            assets::TimImage::parse(mission.interfaceAssets().file(entry.name)),
        });
      }
      auto image = localized ? assets::TimImage::parse(*localized)
                             : assets::TimImage::parse(
                                   mission.interfaceAssets().file(entry.name));
      const auto native_hd_font_layout =
          localized.has_value() && isNativeHdFontSheet(entry.name, image);
      const auto nightvision_layer =
          std::ranges::find(nightvision_scope_layers, entry.name) !=
          nightvision_scope_layers.end();
      const auto regular_hud_layout =
          image.pixels().x >= 768U && image.pixels().y < 256U &&
          static_cast<unsigned int>(image.pixels().x) +
                  image.pixels().width_words <=
              1024U &&
          static_cast<unsigned int>(image.pixels().y) + image.pixels().height <=
              mission_clut_resident_y;
      const auto nightvision_layout =
          nightvision_layer && image.pixels().x >= 512U &&
          image.pixels().y >= 384U &&
          static_cast<unsigned int>(image.pixels().x) +
                  image.pixels().width_words <=
              576U &&
          static_cast<unsigned int>(image.pixels().y) + image.pixels().height <=
              502U;
      if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
          (!regular_hud_layout && !nightvision_layout &&
           !native_hd_font_layout) ||
          image.clut()->x != 768U || image.clut()->y != 483U ||
          image.clut()->width_words != 256U || image.clut()->height != 1U) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "INTERFACE HUD atlas uses an unexpected VRAM layout"};
      }
      if (!assets_.empty() &&
          image.clut()->words != assets_.front().image.clut()->words) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "INTERFACE HUD assets do not share the original palette"};
      }
      assets_.push_back(HudTextureAsset{entry.name, std::move(image)});
    }
    auto armor_pickup = assets::TimImage::parse(
        mission.specialEffects().file(armor_pickup_texture));
    const auto secondary_spfx_palette =
        assets::TimImage::parse(mission.specialEffects().file("BLOT.TIM"));
    if (armor_pickup.mode() != assets::TimPixelMode::indexed8 ||
        !armor_pickup.clut() || armor_pickup.displayWidth() != 32U ||
        armor_pickup.pixels().height != 32U ||
        armor_pickup.clut()->width_words != 256U ||
        armor_pickup.clut()->height != 1U ||
        armor_pickup.clut()->words.empty() || !secondary_spfx_palette.clut() ||
        armor_pickup.clut()->words != secondary_spfx_palette.clut()->words) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "SPFX armour pickup uses an unexpected TIM layout"};
    }
    armor_pickup_palette_ = *armor_pickup.clut();
    armor_pickup_palette_.words.front() = 0U;
    armor_pickup_pixels_ = armor_pickup.pixels();
    const auto keep_vest_pixel = [](std::uint16_t x, std::uint16_t y) {
      if (y < 4U) {
        return (x >= 7U && x <= 12U) || (x >= 19U && x <= 24U);
      }
      if (y < 8U) {
        return (x >= 4U && x <= 12U) || (x >= 19U && x <= 27U);
      }
      if (y < 12U) {
        return x >= 2U && x <= 29U;
      }
      if (y < 26U) {
        return x >= 5U && x <= 26U;
      }
      if (y < 30U) {
        return x >= 7U && x <= 24U;
      }
      return x >= 9U && x <= 22U;
    };
    for (std::uint16_t y = 0U; y < armor_pickup_pixels_.height; ++y) {
      for (std::uint16_t x = 0U; x < armor_pickup.displayWidth(); ++x) {
        if (keep_vest_pixel(x, y)) {
          continue;
        }
        auto &word =
            armor_pickup_pixels_.words[static_cast<std::size_t>(y) *
                                           armor_pickup_pixels_.width_words +
                                       x / 2U];
        word = x % 2U == 0U ? static_cast<std::uint16_t>(word & 0xff00U)
                            : static_cast<std::uint16_t>(word & 0x00ffU);
      }
    }
    assets_.push_back(HudTextureAsset{std::string{armor_pickup_texture},
                                      std::move(armor_pickup)});
    if (assets_.empty()) {
      throw core::Error{core::ErrorCode::not_found,
                        "INTERFACE HUD atlas is empty"};
    }
    static_cast<void>(image("FONTA.TIM"));
    static_cast<void>(image("FONTB.TIM"));
    static_cast<void>(image("FONTC.TIM"));
    static_cast<void>(image("SYMBOL.TIM"));
    static_cast<void>(image("SCOPED.TIM"));
    for (const auto bearing : scope_bearings) {
      static_cast<void>(image(bearing));
    }
    for (const auto layer : nightvision_scope_layers) {
      static_cast<void>(image(layer));
    }
    if (game::russianLanguageActive()) {
      native_font_ = std::make_unique<PsyCrossFontTexture>(
          image("FONTA.TIM"), image("FONTB.TIM"), image("FONTC.TIM"));
      const auto retail_font =
          [&](std::string_view name) -> const assets::TimImage & {
        const auto match = std::ranges::find_if(
            retail_fonts,
            [&](const HudTextureAsset &asset) { return asset.name == name; });
        if (match == retail_fonts.end()) {
          throw core::Error{
              core::ErrorCode::not_found,
              "Original INTERFACE font is missing from the retail atlas: " +
                  std::string{name}};
        }
        return match->image;
      };
      retail_english_font_ = std::make_unique<PsyCrossFontTexture>(
          retail_font("FONTA.TIM"), retail_font("FONTB.TIM"),
          retail_font("FONTC.TIM"));
    }
  }

  [[nodiscard]] const PsyCrossFontTexture *nativeFont() const noexcept {
    return native_font_.get();
  }

  [[nodiscard]] const PsyCrossFontTexture *retailEnglishFont() const noexcept {
    return retail_english_font_.get();
  }

  void invalidate() const noexcept {
    gameplay_state_.reset();
    gameplay_font_resident_ = false;
    gameplay_palette_resident_ = false;
  }

  void restoreGameplay(
      const game::GameplayHud &hud, bool first_person_aim,
      std::span<const game::LegacyDroppedItemBridgeState> dropped_items,
      std::span<const game::GameplayProjectile> projectiles) const {
    const auto weapon = hud.inventory().current();
    const auto scoped_weapon = weapon == game::WeaponId::nightvision_rifle ||
                               weapon == game::WeaponId::sniper_rifle;
    const auto menu_visible = hud.weaponMenuFrames() != 0U;
    auto dropped_item_mask = std::uint32_t{};
    for (const auto &item : dropped_items) {
      if (item.item < game::weapon_slot_count) {
        dropped_item_mask |= std::uint32_t{1U} << item.item;
      } else if (item.item == 0x80U) {
        dropped_item_mask |= std::uint32_t{1U} << game::weapon_slot_count;
      }
    }
    auto projectile_mask = std::uint32_t{};
    for (const auto &projectile : projectiles) {
      if (projectile.active &&
          projectile.phase == game::ProjectilePhase::flying &&
          (projectile.weapon == game::WeaponId::fragmentation_grenade ||
           projectile.weapon == game::WeaponId::gas_grenade)) {
        projectile_mask |= std::uint32_t{1U}
                           << static_cast<std::size_t>(projectile.weapon);
      }
    }
    const auto state = GameplayResidentState{
        weapon,
        menu_visible,
        menu_visible
            ? hud.weaponMenuWindow()
            : std::array<game::WeaponId, game::weapon_menu_slot_count>{},
        first_person_aim && scoped_weapon,
        dropped_item_mask,
        projectile_mask,
    };
    if (gameplay_state_ && *gameplay_state_ == state &&
        gameplay_font_resident_ && gameplay_palette_resident_) {
      return;
    }

    auto uploaded = false;
    const auto upload = [&](std::string_view name) {
      if (name == armor_pickup_texture) {
        uploadTimBlockAt(armor_pickup_pixels_, pickup_resident_x,
                         pickup_resident_y);
      } else {
        uploadHudPixels(image(name).pixels());
      }
      uploaded = true;
    };
    const auto upload_nightvision = [&](std::size_t index) {
      uploadTimBlockAt(image(nightvision_scope_layers[index]).pixels(),
                       nightvision_resident_x[index], nightvision_resident_y);
      uploaded = true;
    };
    // Retail glyph UVs address one page assembled from all three font TIMs.
    // The world pass owns the same transient framebuffer-backed VRAM area, so
    // restoring only FONTA (and only for ammo weapons) leaves glyphs from the
    // other two sheets sampling wall/effect pixels after any gameplay frame.
    // Upload the complete authored font page once here and keep the single
    // palette upload/DrawSync below as the batch boundary.
    if (!gameplay_font_resident_) {
      if (native_font_ == nullptr) {
        upload("FONTA.TIM");
        upload("FONTB.TIM");
        upload("FONTC.TIM");
      }
      gameplay_font_resident_ = true;
    }
    const auto &definition = hud.inventory().currentDefinition();
    for (const auto layer : definition.icon.layers()) {
      upload(layer);
    }
    if (menu_visible) {
      for (const auto menu_weapon : state.menu_window) {
        for (const auto layer :
             game::weaponDefinition(menu_weapon).icon.layers()) {
          upload(layer);
        }
      }
    }
    for (std::size_t item = 0U; item < game::weapon_slot_count; ++item) {
      if ((dropped_item_mask & (std::uint32_t{1U} << item)) == 0U) {
        continue;
      }
      for (const auto layer :
           game::droppedItemIconLayers(static_cast<std::uint16_t>(item))) {
        upload(layer);
      }
    }
    if ((dropped_item_mask & (std::uint32_t{1U} << game::weapon_slot_count)) !=
        0U) {
      upload(armor_pickup_texture);
      uploadTimBlockAt(armor_pickup_palette_, hud_resident_clut_x,
                       pickup_resident_clut_y);
    }
    for (std::size_t weapon_index = 0U; weapon_index < game::weapon_slot_count;
         ++weapon_index) {
      if ((projectile_mask & (std::uint32_t{1U} << weapon_index)) == 0U) {
        continue;
      }
      for (const auto layer :
           game::weaponDefinition(static_cast<game::WeaponId>(weapon_index))
               .icon.layers()) {
        upload(layer);
      }
    }
    if (state.scoped) {
      if (weapon == game::WeaponId::nightvision_rifle) {
        for (std::size_t index = 0; index < nightvision_scope_layers.size();
             ++index) {
          upload_nightvision(index);
        }
      } else {
        upload("SCOPED.TIM");
      }
      for (const auto bearing : scope_bearings) {
        upload(bearing);
      }
    }
    if (!gameplay_palette_resident_) {
      uploadTimBlockAt(*assets_.front().image.clut(), hud_resident_clut_x,
                       hud_resident_clut_y);
      gameplay_palette_resident_ = true;
      uploaded = true;
    }
    if (uploaded) {
      DrawSync(0);
    }
    gameplay_state_ = state;
  }

  void restoreFont() const {
    constexpr std::array names{
        std::string_view{"FONTA.TIM"},
        std::string_view{"FONTB.TIM"},
        std::string_view{"FONTC.TIM"},
        std::string_view{"SYMBOL.TIM"},
    };
    for (const auto name : names) {
      if (native_font_ != nullptr && name.starts_with("FONT")) {
        continue;
      }
      uploadHudPixels(image(name).pixels());
    }
    uploadTimBlockAt(*image("FONTA.TIM").clut(), hud_resident_clut_x,
                     hud_resident_clut_y);
    DrawSync(0);
  }

  void restoreWeaponIcon(game::WeaponId weapon) const {
    for (const auto layer :
         game::droppedItemIconLayers(static_cast<std::uint16_t>(weapon))) {
      uploadHudPixels(image(layer).pixels());
    }
    uploadTimBlockAt(*image("FONTA.TIM").clut(), hud_resident_clut_x,
                     hud_resident_clut_y);
    DrawSync(0);
  }

  [[nodiscard]] const assets::TimImage *
  tryImage(std::string_view name) const noexcept {
    const auto match =
        std::ranges::find_if(assets_, [&](const HudTextureAsset &asset) {
          return asset.name == name;
        });
    return match == assets_.end() ? nullptr : &match->image;
  }

  [[nodiscard]] const assets::TimImage &image(std::string_view name) const {
    const auto *result = tryImage(name);
    if (result == nullptr) {
      throw core::Error{core::ErrorCode::not_found,
                        "HUD texture is missing from the retail atlases: " +
                            std::string{name}};
    }
    return *result;
  }

  [[nodiscard]] std::optional<std::uint16_t>
  fontPaletteWord(std::uint8_t page_u, std::uint8_t page_v) const noexcept {
    const auto &font_page = image("FONTA.TIM");
    const auto page_x = static_cast<int>(font_page.pixels().x &
                                         static_cast<std::uint16_t>(~63U));
    const auto page_y = static_cast<int>(font_page.pixels().y &
                                         static_cast<std::uint16_t>(~255U));
    constexpr std::array names{
        std::string_view{"FONTA.TIM"},
        std::string_view{"FONTB.TIM"},
        std::string_view{"FONTC.TIM"},
    };
    for (const auto name : names) {
      const auto &font = image(name);
      const auto &pixels = font.pixels();
      const auto first_u = (static_cast<int>(pixels.x) - page_x) * 2;
      const auto first_v = static_cast<int>(pixels.y) - page_y;
      const auto local_u = static_cast<int>(page_u) - first_u;
      const auto local_v = static_cast<int>(page_v) - first_v;
      if (local_u < 0 || local_v < 0 ||
          local_u >= static_cast<int>(font.displayWidth()) ||
          local_v >= static_cast<int>(font.displayHeight())) {
        continue;
      }
      const auto word =
          pixels.words[static_cast<std::size_t>(local_v) * pixels.width_words +
                       static_cast<std::size_t>(local_u / 2)];
      const auto palette_index = static_cast<std::uint8_t>(
          local_u % 2 == 0 ? word & 0xffU : word >> 8U);
      return font.clut()->words[palette_index];
    }
    return std::nullopt;
  }

private:
  struct GameplayResidentState {
    game::WeaponId weapon{};
    bool menu_visible{};
    std::array<game::WeaponId, game::weapon_menu_slot_count> menu_window{};
    bool scoped{};
    std::uint32_t dropped_item_mask{};
    std::uint32_t projectile_mask{};

    [[nodiscard]] friend bool
    operator==(const GameplayResidentState &,
               const GameplayResidentState &) noexcept = default;
  };

  std::vector<HudTextureAsset> assets_;
  std::unique_ptr<PsyCrossFontTexture> native_font_;
  std::unique_ptr<PsyCrossFontTexture> retail_english_font_;
  assets::TimBlock armor_pickup_pixels_;
  assets::TimBlock armor_pickup_palette_;
  mutable std::optional<GameplayResidentState> gameplay_state_;
  mutable bool gameplay_font_resident_{};
  mutable bool gameplay_palette_resident_{};
};

struct PauseTextureAsset {
  std::string name;
  assets::TimImage image;
};

class PauseTextureAtlas final {
public:
  explicit PauseTextureAtlas(const game::MissionPackage &mission) {
    for (const auto &entry : mission.menuAssets().entries()) {
      if (!std::string_view{entry.name}.ends_with(".TIM")) {
        continue;
      }
      const auto map_asset = std::string_view{entry.name}.starts_with("MAP");
      auto localized =
          map_asset ? game::readLocalizedAsset(
                          "maps/" + std::to_string(mission.definition().index) +
                          "/" + entry.name)
                    : std::nullopt;
      assets_.push_back(PauseTextureAsset{
          entry.name,
          localized
              ? assets::TimImage::parse(*localized)
              : assets::TimImage::parse(mission.menuAssets().file(entry.name)),
      });
    }
    if (image("GLOKSIL.TIM") == nullptr) {
      throw core::Error{
          core::ErrorCode::not_found,
          "MENU.HOG does not contain the universal pause weapon texture"};
    }
  }

  [[nodiscard]] const assets::TimImage *
  image(std::string_view name) const noexcept {
    const auto match =
        std::ranges::find_if(assets_, [&](const PauseTextureAsset &asset) {
          return asset.name == name;
        });
    return match == assets_.end() ? nullptr : &match->image;
  }

private:
  std::vector<PauseTextureAsset> assets_;
};

class PoliceLightbarAnimation final {
public:
  void invalidate() noexcept {
    resident_copies_.clear();
    touched_pages_.clear();
    input_presentation_sequence_ = std::numeric_limits<std::uint64_t>::max();
    input_tick_ = std::numeric_limits<std::uint64_t>::max();
    residency_revision_ = 0U;
    inputs_valid_ = false;
  }

  [[nodiscard]] bool
  synchronize(const game::GameplaySession &gameplay, std::uint64_t tick,
              TextureStreamer &textures,
              std::span<const game::LegacyGuestSpritePresentationState>
                  retained_sprites = {}) {
    const auto presentation_sequence = gameplay.legacyPresentationSequence();
    const auto residency_revision = textures.residencyRevision();
    if (inputs_valid_ && input_tick_ == tick &&
        input_presentation_sequence_ == presentation_sequence &&
        residency_revision_ == residency_revision) {
      return false;
    }

    const auto &frame = game::policeLightbarFrame(tick);
    std::vector<game::EffectTextureCopy> required_copies;
    for (const auto object_index : gameplay.activeObjects()) {
      const auto *model = gameplay.displayedObjectModel(object_index);
      if (model == nullptr ||
          model->visual_effect != game::ObjectVisualEffect::police_lightbar) {
        continue;
      }
      const auto bank = gameplay.displayedObjectTextureBank(object_index);
      const auto blue = textures.relocateEffectTextureCopy(frame.blue, bank);
      const auto red = textures.relocateEffectTextureCopy(frame.red, bank);
      // Never fall back to retail physical TP10: that slot may currently be
      // owned by a different bank after native alias allocation.
      if (!blue || !red) {
        required_copies.clear();
        break;
      }
      for (const auto &copy : std::array{*blue, *red}) {
        if (std::ranges::find(required_copies, copy) == required_copies.end()) {
          required_copies.push_back(copy);
        }
      }
    }
    if (required_copies == resident_copies_ &&
        residency_revision_ == residency_revision) {
      input_tick_ = tick;
      input_presentation_sequence_ = presentation_sequence;
      inputs_valid_ = true;
      return false;
    }

    for (const auto page : touched_pages_) {
      textures.invalidatePhysicalPage(page);
    }
    if (!touched_pages_.empty()) {
      textures.ensure(gameplay, retained_sprites);
    }
    touched_pages_.clear();
    resident_copies_.clear();
    if (required_copies.empty()) {
      residency_revision_ = textures.residencyRevision();
      input_tick_ = tick;
      input_presentation_sequence_ = presentation_sequence;
      inputs_valid_ = true;
      return false;
    }

    const auto apply_copy = [](const game::EffectTextureCopy &texture_copy) {
      auto source = RECT16{
          texture_copy.source.x,
          texture_copy.source.y,
          texture_copy.source.width,
          texture_copy.source.height,
      };
      static_cast<void>(MoveImage(&source, texture_copy.destination_x,
                                  texture_copy.destination_y));
    };
    for (const auto &copy : required_copies) {
      apply_copy(copy);
      const auto page =
          static_cast<unsigned int>(copy.destination_x / 64) +
          (static_cast<unsigned int>(copy.destination_y / 256) * 16U);
      if (std::ranges::find(touched_pages_, page) == touched_pages_.end()) {
        touched_pages_.push_back(page);
      }
    }
    DrawSync(0);
    for (const auto page : touched_pages_) {
      textures.captureExpectedPhysicalPage(page);
    }
    resident_copies_ = std::move(required_copies);
    residency_revision_ = textures.residencyRevision();
    input_tick_ = tick;
    input_presentation_sequence_ = presentation_sequence;
    inputs_valid_ = true;
    return true;
  }

private:
  std::vector<game::EffectTextureCopy> resident_copies_;
  std::vector<unsigned int> touched_pages_;
  std::uint64_t residency_revision_{};
  std::uint64_t input_presentation_sequence_{};
  std::uint64_t input_tick_{};
  bool inputs_valid_{};
};

class RetailScrimAnimation final {
public:
  void reset() noexcept {
    executed_copy_count_ = 0U;
    scene_room_.reset();
    moves_.clear();
    for (auto &pages : bank_pages_) {
      pages.clear();
    }
    copy_scratch_.clear();
  }

  void restore(const game::GameplaySession &gameplay,
               const game::LegacyScrimBridgeState &state,
               TextureStreamer &textures) {
    if (!state.resource_present) {
      reset();
      return;
    }
    const auto moves =
        !state.vram_moves.empty()
            ? std::span<const game::LegacyVramMoveBridgeState>{state.vram_moves}
            : std::span<const game::LegacyVramMoveBridgeState>{moves_};
    if (!moves.empty()) {
      ensureState(moves, gameplay.currentRoom(), textures);
    }
    if (synchronizeResidentPages(textures)) {
      DrawSync(0);
    }
  }

  [[nodiscard]] bool
  prepareFrame(const game::GameplaySession &gameplay,
               const game::LegacyScrimBridgeState &state,
               std::span<const game::LegacyScrimCopyPresentationPhase> phases,
               std::uint64_t displayed_guest_frame, TextureStreamer &textures) {
    return synchronize(gameplay, state, phases, displayed_guest_frame,
                       PhaseSelection::preceding, textures);
  }

  [[nodiscard]] bool
  commitFrame(const game::GameplaySession &gameplay,
              const game::LegacyScrimBridgeState &state,
              std::span<const game::LegacyScrimCopyPresentationPhase> phases,
              std::uint64_t displayed_guest_frame, TextureStreamer &textures) {
    return synchronize(gameplay, state, phases, displayed_guest_frame,
                       PhaseSelection::displayed, textures);
  }

  [[nodiscard]] bool commitSkippedFrame(
      const game::GameplaySession &gameplay,
      const game::LegacyScrimBridgeState &state,
      std::span<const game::LegacyScrimCopyPresentationPhase> phases,
      std::uint64_t displayed_guest_frame, TextureStreamer &textures) {
    return synchronize(gameplay, state, phases, displayed_guest_frame,
                       PhaseSelection::completed, textures);
  }

private:
  enum class PhaseSelection : std::uint8_t {
    preceding,
    displayed,
    completed,
  };

  [[nodiscard]] bool
  synchronize(const game::GameplaySession &gameplay,
              const game::LegacyScrimBridgeState &state,
              std::span<const game::LegacyScrimCopyPresentationPhase> phases,
              std::uint64_t displayed_guest_frame, PhaseSelection selection,
              TextureStreamer &textures) {
    if (!state.resource_present) {
      reset();
      return false;
    }
    if (std::ranges::any_of(phases, [displayed_guest_frame](const auto &phase) {
          return game::legacyScrimCopyPhasePosition(phase.guest_frame,
                                                    displayed_guest_frame) ==
                 game::LegacyScrimCopyPhasePosition::future_frame;
        })) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "SCRIM copy phase is newer than its displayed frame"};
    }
    const auto moves =
        !state.vram_moves.empty()
            ? std::span<const game::LegacyVramMoveBridgeState>{state.vram_moves}
        : phases.empty() ? std::span<const game::LegacyVramMoveBridgeState>{}
                         : std::span<const game::LegacyVramMoveBridgeState>{
                               phases.back().moves};
    if (moves.empty()) {
      const auto uploaded = synchronizeResidentPages(textures);
      if (uploaded) {
        DrawSync(0);
      }
      return uploaded;
    }
    ensureState(moves, gameplay.currentRoom(), textures);
    auto changed = synchronizeResidentPages(textures);
    if (changed) {
      DrawSync(0);
    }

    const auto selected = [displayed_guest_frame,
                           selection](const auto &phase) {
      const auto position = game::legacyScrimCopyPhasePosition(
          phase.guest_frame, displayed_guest_frame);
      switch (selection) {
      case PhaseSelection::preceding:
        return position ==
               game::LegacyScrimCopyPhasePosition::preceding_display;
      case PhaseSelection::displayed:
        return position == game::LegacyScrimCopyPhasePosition::displayed_frame;
      case PhaseSelection::completed:
        return position != game::LegacyScrimCopyPhasePosition::future_frame;
      }
      return false;
    };
    const auto has_selected = std::ranges::any_of(phases, selected);
    // Retail links DR_MOVE at OT depth 1 after world/SCRIM geometry. A phase
    // belonging to the displayed tick therefore sees the old texture and
    // commits only after that frame's GPU work. Catch-up phases from older
    // guest ticks are applied before drawing the current frame.
    if (has_selected && selection != PhaseSelection::preceding) {
      DrawSync(0);
    }
    for (const auto &phase : phases) {
      if (!selected(phase)) {
        continue;
      }
      if (!std::ranges::equal(phase.moves, moves_)) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "Retail SCRIM copy phase changed its authored packet ring"};
      }
      apply(moves_);
      executed_copy_count_ =
          (executed_copy_count_ + 1U) % game::legacy_scrim_copy_cycle;
      changed = true;
    }
    if (has_selected) {
      static_cast<void>(uploadDestinationPages(textures));
      DrawSync(0);
    }
    return changed;
  }

  struct CpuPage {
    unsigned int logical_page{};
    bool destination{};
    std::array<std::byte, texture_page_bytes> bytes{};
    std::optional<std::uint64_t> uploaded_residency;
  };

  void ensureState(std::span<const game::LegacyVramMoveBridgeState> moves,
                   std::size_t scene_room, const TextureStreamer &textures) {
    if (!bank_pages_[0].empty() && scene_room_ == scene_room &&
        std::ranges::equal(moves_, moves)) {
      return;
    }

    const auto same_packet_ring = std::ranges::equal(moves_, moves);
    if (!same_packet_ring) {
      executed_copy_count_ = 0U;
    }
    scene_room_ = scene_room;
    moves_.assign(moves.begin(), moves.end());
    for (auto bank = 0; bank < 2; ++bank) {
      auto &pages = bank_pages_[static_cast<std::size_t>(bank)];
      pages.clear();
      const auto append = [&](int x, int y, int width, int height,
                              bool destination) {
        const auto local_x = x & 63;
        const auto local_y = y & 255;
        if (local_x + width > 64 || local_y + height > 256) {
          throw core::Error{
              core::ErrorCode::invalid_format,
              "Retail SCRIM copy crosses a logical texture-page boundary"};
        }
        const auto page = static_cast<unsigned int>(x / 64) +
                          static_cast<unsigned int>(y / 256) * 16U;
        auto existing = std::ranges::find_if(pages, [page](const auto &entry) {
          return entry.logical_page == page;
        });
        if (existing != pages.end()) {
          existing->destination = existing->destination || destination;
          return;
        }
        const auto authored = textures.authoredTexturePage(page, bank);
        if (authored.size() != texture_page_bytes) {
          throw core::Error{
              core::ErrorCode::invalid_format,
              "Retail SCRIM texture page has invalid source data"};
        }
        pages.push_back(CpuPage{page, destination});
        std::ranges::copy(authored, pages.back().bytes.begin());
      };
      for (const auto &move : moves) {
        append(move.source_x, move.source_y, move.width, move.height, false);
        append(move.destination_x, move.destination_y, move.width, move.height,
               true);
      }
    }
    rebuildAuthoredPhase(textures);
    if (textureDiagnosticsEnabled()) {
      PsyX_Log_Info(
          "[TextureDiag][scrim-state] banks=2 room=%zu phase=%llu moves=%zu\n",
          *scene_room_, static_cast<unsigned long long>(executed_copy_count_),
          moves_.size());
    }
  }

  [[nodiscard]] CpuPage &page(int bank, unsigned int logical_page) {
    auto &pages = bank_pages_[static_cast<std::size_t>(bank)];
    const auto found =
        std::ranges::find_if(pages, [logical_page](const CpuPage &candidate) {
          return candidate.logical_page == logical_page;
        });
    if (found == pages.end()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Retail SCRIM copy references an unknown CPU page"};
    }
    return *found;
  }

  void applyBank(int bank,
                 std::span<const game::LegacyVramMoveBridgeState> moves) {
    for (const auto &move : moves) {
      const auto source_page =
          static_cast<unsigned int>(move.source_x / 64) +
          static_cast<unsigned int>(move.source_y / 256) * 16U;
      const auto destination_page =
          static_cast<unsigned int>(move.destination_x / 64) +
          static_cast<unsigned int>(move.destination_y / 256) * 16U;
      const auto source_x = static_cast<unsigned int>(move.source_x & 63);
      const auto source_y = static_cast<unsigned int>(move.source_y & 255);
      const auto destination_x =
          static_cast<unsigned int>(move.destination_x & 63);
      const auto destination_y =
          static_cast<unsigned int>(move.destination_y & 255);
      const auto width = static_cast<unsigned int>(move.width);
      const auto height = static_cast<unsigned int>(move.height);
      if (source_x + width > 64U || destination_x + width > 64U ||
          source_y + height > 256U || destination_y + height > 256U) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "Retail SCRIM copy crosses a logical texture-page boundary"};
      }

      const auto row_bytes = width * sizeof(std::uint16_t);
      copy_scratch_.resize(row_bytes * height);
      const auto &source = page(bank, source_page).bytes;
      auto &destination = page(bank, destination_page).bytes;
      copyTexturePageRectangle(source, source_x, source_y, destination,
                               destination_x, destination_y, width, height,
                               copy_scratch_);
    }
  }

  void apply(std::span<const game::LegacyVramMoveBridgeState> moves) {
    for (auto bank = 0; bank < 2; ++bank) {
      applyBank(bank, moves);
    }
  }

  void rebuildAuthoredPhase(const TextureStreamer &textures) {
    for (auto bank = 0; bank < 2; ++bank) {
      for (auto &page_state : bank_pages_[static_cast<std::size_t>(bank)]) {
        const auto authored =
            textures.authoredTexturePage(page_state.logical_page, bank);
        if (authored.size() != page_state.bytes.size()) {
          throw core::Error{core::ErrorCode::invalid_format,
                            "Retail SCRIM page has invalid authored data"};
        }
        std::ranges::copy(authored, page_state.bytes.begin());
        page_state.uploaded_residency.reset();
      }
    }
    // The copy phase is global retail VRAM state. Replay the complete packet
    // ring so every destination page comes from the same phase.
    for (auto phase = std::uint64_t{}; phase < executed_copy_count_; ++phase) {
      apply(moves_);
    }
  }

  [[nodiscard]] bool uploadDestinationPages(TextureStreamer &textures) {
    auto uploaded = false;
    for (auto bank = 0; bank < 2; ++bank) {
      for (auto &page_state : bank_pages_[static_cast<std::size_t>(bank)]) {
        if (!page_state.destination) {
          continue;
        }
        const auto token =
            textures.residentPageToken(page_state.logical_page, bank);
        if (!token) {
          page_state.uploaded_residency.reset();
          continue;
        }
        if (textures.uploadRuntimeTexturePage(page_state.logical_page, bank,
                                              page_state.bytes)) {
          page_state.uploaded_residency = token;
          uploaded = true;
        }
      }
    }
    return uploaded;
  }

  [[nodiscard]] bool synchronizeResidentPages(TextureStreamer &textures) {
    auto residency_changed = false;
    for (auto bank = 0; bank < 2; ++bank) {
      for (auto &page_state : bank_pages_[static_cast<std::size_t>(bank)]) {
        if (!page_state.destination) {
          continue;
        }
        const auto token =
            textures.residentPageToken(page_state.logical_page, bank);
        if (!token) {
          page_state.uploaded_residency.reset();
          continue;
        }
        if (page_state.uploaded_residency == token) {
          continue;
        }
        residency_changed = true;
        if (textureDiagnosticsEnabled()) {
          PsyX_Log_Info(
              "[TextureDiag][scrim-residency-change] logical=%u bank=%d "
              "token=%llu phase=%llu\n",
              page_state.logical_page, bank,
              static_cast<unsigned long long>(*token),
              static_cast<unsigned long long>(executed_copy_count_));
        }
      }
    }
    if (!residency_changed) {
      return false;
    }

    // Rebuild and publish the entire ring atomically. Restoring just the page
    // whose token changed left train/sign polygons sampling different SCRIM
    // phases, which looked like textures assigned to the wrong mesh regions.
    rebuildAuthoredPhase(textures);
    return uploadDestinationPages(textures);
  }

  std::uint64_t executed_copy_count_{};
  std::optional<std::size_t> scene_room_;
  std::vector<game::LegacyVramMoveBridgeState> moves_;
  std::array<std::vector<CpuPage>, 2U> bank_pages_;
  std::vector<std::byte> copy_scratch_;
};
