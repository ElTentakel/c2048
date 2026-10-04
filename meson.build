project('c2048', 'c',
  version: '1.0.0',
  license: 'MIT',
  meson_version: '>= 1.0',
  default_options: ['c_std=gnu11', 'warning_level=2'])

app_id = 'io.github.ElTentakel.c2048'
gnome = import('gnome')

res = gnome.compile_resources('resources', 'src/c2048.gresource.xml', source_dir: 'src')
executable('c2048', 'src/main.c', res,
  dependencies: [dependency('gtk4'), dependency('libadwaita-1'),
                 meson.get_compiler('c').find_library('m')],
  install: true)

install_data('data' / app_id + '.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data' / app_id + '.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo')
install_data('data/icons' / app_id + '.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')

desktop_validate = find_program('desktop-file-validate', required: false)
if desktop_validate.found()
  test('validate-desktop', desktop_validate, args: [files('data' / app_id + '.desktop')])
endif
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('validate-metainfo', appstreamcli,
    args: ['validate', '--no-net', '--explain', files('data' / app_id + '.metainfo.xml')])
endif
