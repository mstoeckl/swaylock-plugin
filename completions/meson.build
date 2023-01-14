bash_comp = dependency('bash-completion', required: false)
fish_comp = dependency('fish', required: false)

datadir = get_option('datadir')

if get_option('zsh-completions')
	zsh_install_dir = datadir + '/zsh/site-functions'

	install_data(['zsh/_swaylock'], rename: ['zsh/_swaylock-plugin'], install_dir: zsh_install_dir)
endif

if get_option('bash-completions')
	if bash_comp.found()
		bash_install_dir = bash_comp.get_variable('completionsdir')
	else
		bash_install_dir = datadir + '/bash-completion/completions'
	endif

	install_data(['bash/swaylock'], rename: ['bash/swaylock-plugin'], install_dir: bash_install_dir)
endif

if get_option('fish-completions')
	if fish_comp.found()
		fish_install_dir = fish_comp.get_variable('completionsdir')
	else
		fish_install_dir = datadir + '/fish/vendor_completions.d'
	endif

	install_data(['fish/swaylock.fish'], rename: ['fish/swaylock-plugin.fish'], install_dir: fish_install_dir)
endif
