project(
  'gdk-pixbuf-loader-ktx',
  ['c'],
  default_options : ['buildtype=debugoptimized']
)

src = [
  'io-ktx.c'
]

gdk_pb_dep = dependency('gdk-pixbuf-2.0', version: '>2.22.0', method: 'pkg-config')
gdk_pb_loaderdir = gdk_pb_dep.get_pkgconfig_variable('gdk_pixbuf_moduledir')
gdk_pb_query_loaders = gdk_pb_dep.get_pkgconfig_variable('gdk_pixbuf_query_loaders')

ktx_sp = subproject('ktx', default_options : ['buildtype=release'])
ktx_dep = ktx_sp.get_variable('ktx_dep')

shared_module(
  'pixbufloader-ktx',
  ['io-ktx.c'],
  dependencies: [ktx_dep, gdk_pb_dep],
  install: true,
  install_dir: gdk_pb_loaderdir
)

gdk_pb_query_loaders = find_program(get_option('gdk_pixbuf_query_loaders_path'), gdk_pb_query_loaders, gdk_pb_query_loaders+'-32', gdk_pb_query_loaders+'-64')
meson.add_install_script(gdk_pb_query_loaders.path(), '--update-cache')
