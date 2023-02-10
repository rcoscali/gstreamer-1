# Discret11 Scrambler/Descrambler: Process audio & video
# Used Carlos Rafael Giani rawparse element as a base for this one
# Credits & Thanks goes to him for the rawparse original design 
discret11_sources = [
  'gstdiscret11parse.c',
  'gstdiscret11audioparse.c',
  'gstdiscret11videoparse.c',
  'gstunalignedaudioparse.c',
  'gstunalignedvideoparse.c',
]

gstdiscret11 = library('gstdiscret11',
  discret11_sources,
  c_args : gst_plugins_base_args,
  include_directories: [configinc],
  dependencies : [gst_base_dep, video_dep, audio_dep],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gstdiscret11, install_dir : plugins_pkgconfig_install_dir)
plugins += [gstdiscret11]
