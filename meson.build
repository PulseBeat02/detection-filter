project('vlc-facedetection', 'cpp',
        version: '1.0.0',
        default_options: ['cpp_std=c++17', 'warning_level=3']
)

cc  = meson.get_compiler('cpp')
inc = include_directories('.')

openmp_dep = dependency('openmp', required: get_option('openmp'))

cpp_args = []
if get_option('extra_warnings')
    cpp_args += ['-Wall', '-Wextra', '-Wpedantic']
endif
if get_option('enable_avx2')
    cpp_args += ['-D_ENABLE_AVX2', '-mavx2', '-mfma']
endif
if get_option('enable_avx512')
    cpp_args += ['-D_ENABLE_AVX512', '-mavx512f', '-mavx512bw', '-mavx512dq', '-mavx512vl']
endif
if get_option('enable_neon')
    cpp_args += ['-D_ENABLE_NEON']
endif
if host_machine.system() == 'darwin'
    cpp_args += ['-fvisibility=hidden']
endif

common_deps = []
if openmp_dep.found()
    common_deps += openmp_dep
endif

face_srcs = [
    'facedetectcnn.cpp',
    'facedetectcnn-model.cpp',
    'facedetectcnn-data.cpp',
]

face_lib = static_library(
    'facedetection',
    face_srcs,
    include_directories: inc,
    cpp_args: cpp_args,
    dependencies: common_deps,
    pic: true,
    install: false
)

build_plugin = get_option('build_vlc_plugin')

if build_plugin
    vlc_deps = []
    vlc_plugin_dep = dependency('vlc-plugin', required: false, method: 'pkg-config')
    if vlc_plugin_dep.found()
        vlc_deps += vlc_plugin_dep
    else
        vlccore_dep = dependency('vlccore', required: true, method: 'pkg-config')
        vlc_deps += vlccore_dep
    endif

    plugin_root = get_option('vlc_plugins_dir')
    if plugin_root == ''
        plugin_root = meson.project_build_root() / 'vlc-plugins'
    endif
    plugin_install_dir = plugin_root / 'video_filter'

    shared_module(
        'facedetection',
        ['facedetection.cpp'],
        include_directories: inc,
        link_with: face_lib,
        dependencies: vlc_deps + common_deps,
        name_suffix: 'plugin',
        install: true,
        install_dir: plugin_install_dir
    )
else
    message('VLC plugin skipped: -Dbuild_vlc_plugin=true to enable.')
endif

summary({
            'OpenMP': openmp_dep.found(),
            'AVX2': get_option('enable_avx2'),
            'AVX512': get_option('enable_avx512'),
            'NEON': get_option('enable_neon'),
            'Build VLC plugin': build_plugin,
        }, section: 'Build configuration', bool_yn: true)
