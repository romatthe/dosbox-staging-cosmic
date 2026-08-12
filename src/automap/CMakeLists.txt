target_sources(dosboxcommon PRIVATE
  automap.cpp
  automap_wiz6.cpp
  automap_wiz6_atlas.cpp
  automap_wiz6_render.cpp
)

target_link_libraries(dosboxcommon PRIVATE imgui)
