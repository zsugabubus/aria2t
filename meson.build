project('aria2t', 'c',
	version: run_command(
		'git describe --always --tags --dirty --match v*'.split(' ')
	).stdout().strip(),
	default_options: [
		'c_std=c11',
		'warning_level=2',
	],
)

# subprojects require meson.build that I do not want to mess with.
run_command('git submodule update --init --recursive'.split(' '))

add_project_arguments([
	'-Wno-override-init',
	'-DVERSION="@0@"'.format(meson.project_version()),
], language: 'c')

executable('aria2t',
	'aria2t.c',
	'program.c',
	'websocket.c',
	'b64.c',
	'fourmat/fourmat.c',
	'jeezson/jeezson.c',
	custom_target(
		'keys',
		input: ['aria2t.c'],
		output: 'keys.h',
		command: ['keys.gen', '@INPUT@'],
		capture: true,
	),
	dependencies: dependency('ncursesw'),
	install: true,
)

custom_target(
	'aria2t man',
	input: ['aria2t.1.template', 'aria2t.c'],
	output: 'aria2t.1',
	command: ['manpage.gen', '@INPUT@'],
	capture: true,
	install: true,
	install_dir: join_paths(get_option('mandir'), 'man1')
)
