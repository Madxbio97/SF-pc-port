namespace {

struct PsyXSceneFogParameters {
  bool enabled = false;
  bool drawEnabled = false;
  float color[3]{};
  float dqa = 0.0F;
  float dqb = 0.0F;
  float projection = 0.0F;
  float terrainThreshold = 0.0F;
  float terrainScale = 0.0F;
  float useGte = 0.0F;
  unsigned int revision = 1U;
};

PsyXSceneFogParameters g_sceneFog;

void PsyX_AdvanceSceneFogRevision() {
  ++g_sceneFog.revision;
  if (g_sceneFog.revision == 0U)
    g_sceneFog.revision = 1U;
}

void PsyX_InitialiseAtmosphere() {
  if (g_cfg_volumetricFog)
    eprintf("*Scene atmosphere: per-fragment volumetric fog enabled\n");
}

void PsyX_DestroyAtmosphere() {}

#if USE_OPENGL
void PsyX_ApplySceneFogUniforms(PSXGPU_Shader *shader) {
  if (shader == NULL ||
      shader->appliedSceneFogRevision == g_sceneFog.revision)
    return;

  const float enabled = g_cfg_volumetricFog && g_sceneFog.enabled &&
                                g_sceneFog.drawEnabled
                            ? 1.0F
                            : 0.0F;
  glUniform4f(shader->sceneFogColorEnabledLoc, g_sceneFog.color[0],
              g_sceneFog.color[1], g_sceneFog.color[2], enabled);
  glUniform4f(shader->sceneFogGteLoc, g_sceneFog.dqa, g_sceneFog.dqb,
              g_sceneFog.projection, g_sceneFog.useGte);
  glUniform2f(shader->sceneFogTerrainLoc, g_sceneFog.terrainThreshold,
              g_sceneFog.terrainScale);
  shader->appliedSceneFogRevision = g_sceneFog.revision;
}
#endif

} // namespace

void GR_SetSceneFogParameters(int enable, unsigned char red,
                              unsigned char green, unsigned char blue, int dqa,
                              int dqb, int projection,
                              unsigned int terrainDepthCue) {
  const unsigned int threshold =
      terrainDepthCue == 0U ? 0x07ffU : terrainDepthCue & 0xffffU;
  const unsigned int shift = terrainDepthCue >> 16U;
  const bool terrainValid = threshold < 0x1000U && shift <= 15U;
  g_sceneFog.enabled =
      enable != 0 && projection > 0 && (dqa != 0 || terrainValid);
  g_sceneFog.color[0] = static_cast<float>(red) / 255.0F;
  g_sceneFog.color[1] = static_cast<float>(green) / 255.0F;
  g_sceneFog.color[2] = static_cast<float>(blue) / 255.0F;
  g_sceneFog.dqa = static_cast<float>(dqa);
  g_sceneFog.dqb = static_cast<float>(dqb);
  g_sceneFog.projection = static_cast<float>(projection);
  g_sceneFog.terrainThreshold = static_cast<float>(threshold);
  g_sceneFog.terrainScale =
      terrainValid ? static_cast<float>(1U << shift) / 4096.0F : 0.0F;
  g_sceneFog.useGte = dqa != 0 ? 1.0F : 0.0F;
  PsyX_AdvanceSceneFogRevision();
}

void GR_EnableSceneFog(int enable) {
  const bool drawEnabled = enable != 0;
  if (g_sceneFog.drawEnabled == drawEnabled)
    return;
  g_sceneFog.drawEnabled = drawEnabled;
  PsyX_AdvanceSceneFogRevision();
}
