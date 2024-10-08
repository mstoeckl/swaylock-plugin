# swaylock-plugin(1) completion

complete -c swaylock-plugin -l bs-hl-color                 --description "Sets the color of backspace highlight segments."
complete -c swaylock-plugin -l caps-lock-bs-hl-color       --description "Sets the color of backspace highlight segments when Caps Lock is active."
complete -c swaylock-plugin -l caps-lock-key-hl-color      --description "Sets the color of the key press highlight segments when Caps Lock is active."
complete -c swaylock-plugin -l color                  -s c --description "Turn the screen into the given color instead of white."
complete -c swaylock-plugin -l config                 -s C --description "Path to the config file."
complete -c swaylock-plugin -l daemonize              -s f --description "Detach from the controlling terminal after locking."
complete -c swaylock-plugin -l debug                  -s d --description "Enable debugging output."
complete -c swaylock-plugin -l disable-caps-lock-text -s L --description "Disable the Caps Lock text."
complete -c swaylock-plugin -l font                        --description "Sets the font of the text."
complete -c swaylock-plugin -l font-size                   --description "Sets a fixed font size for the indicator text."
complete -c swaylock-plugin -l help                   -s h --description "Show help message and quit."
complete -c swaylock-plugin -l hide-keyboard-layout   -s K --description "Hide the current xkb layout while typing."
complete -c swaylock-plugin -l ignore-empty-password  -s e --description "When an empty password is provided, do not validate it."
complete -c swaylock-plugin -l image                  -s i --description "Display the given image, optionally only on the given output."
complete -c swaylock-plugin -l indicator-caps-lock    -s l --description "Show the current Caps Lock state also on the indicator."
complete -c swaylock-plugin -l indicator-idle-visible      --description "Sets the indicator to show even if idle."
complete -c swaylock-plugin -l indicator-radius            --description "Sets the indicator radius."
complete -c swaylock-plugin -l indicator-thickness         --description "Sets the indicator thickness."
complete -c swaylock-plugin -l indicator-x-position        --description "Sets the horizontal position of the indicator."
complete -c swaylock-plugin -l indicator-y-position        --description "Sets the vertical position of the indicator."
complete -c swaylock-plugin -l inside-caps-lock-color      --description "Sets the color of the inside of the indicator when Caps Lock is active."
complete -c swaylock-plugin -l inside-clear-color          --description "Sets the color of the inside of the indicator when cleared."
complete -c swaylock-plugin -l inside-color                --description "Sets the color of the inside of the indicator."
complete -c swaylock-plugin -l inside-ver-color            --description "Sets the color of the inside of the indicator when verifying."
complete -c swaylock-plugin -l inside-wrong-color          --description "Sets the color of the inside of the indicator when invalid."
complete -c swaylock-plugin -l key-hl-color                --description "Sets the color of the key press highlight segments."
complete -c swaylock-plugin -l layout-bg-color             --description "Sets the background color of the box containing the layout text."
complete -c swaylock-plugin -l layout-border-color         --description "Sets the color of the border of the box containing the layout text."
complete -c swaylock-plugin -l layout-text-color           --description "Sets the color of the layout text."
complete -c swaylock-plugin -l line-caps-lock-color        --description "Sets the color of the line between the inside and ring when Caps Lock is active."
complete -c swaylock-plugin -l line-clear-color            --description "Sets the color of the line between the inside and ring when cleared."
complete -c swaylock-plugin -l line-color                  --description "Sets the color of the line between the inside and ring."
complete -c swaylock-plugin -l line-uses-inside       -s n --description "Use the inside color for the line between the inside and ring."
complete -c swaylock-plugin -l line-uses-ring         -s r --description "Use the ring color for the line between the inside and ring."
complete -c swaylock-plugin -l line-ver-color              --description "Sets the color of the line between the inside and ring when verifying."
complete -c swaylock-plugin -l line-wrong-color            --description "Sets the color of the line between the inside and ring when invalid."
complete -c swaylock-plugin -l no-unlock-indicator    -s u --description "Disable the unlock indicator."
complete -c swaylock-plugin -l ring-caps-lock-color        --description "Sets the color of the ring of the indicator when Caps Lock is active."
complete -c swaylock-plugin -l ring-clear-color            --description "Sets the color of the ring of the indicator when cleared."
complete -c swaylock-plugin -l ring-color                  --description "Sets the color of the ring of the indicator."
complete -c swaylock-plugin -l ring-ver-color              --description "Sets the color of the ring of the indicator when verifying."
complete -c swaylock-plugin -l ring-wrong-color            --description "Sets the color of the ring of the indicator when invalid."
complete -c swaylock-plugin -l scaling                -s s --description "Image scaling mode: stretch, fill, fit, center, tile, solid_color."
complete -c swaylock-plugin -l separator-color             --description "Sets the color of the lines that separate highlight segments."
complete -c swaylock-plugin -l show-failed-attempts   -s F --description "Show current count of failed authentication attempts."
complete -c swaylock-plugin -l show-keyboard-layout   -s k --description "Display the current xkb layout while typing."
complete -c swaylock-plugin -l text-caps-lock-color        --description "Sets the color of the text when Caps Lock is active."
complete -c swaylock-plugin -l text-clear-color            --description "Sets the color of the text when cleared."
complete -c swaylock-plugin -l text-color                  --description "Sets the color of the text."
complete -c swaylock-plugin -l text-ver-color              --description "Sets the color of the text when verifying."
complete -c swaylock-plugin -l text-wrong-color            --description "Sets the color of the text when invalid."
complete -c swaylock-plugin -l tiling                 -s t --description "Same as --scaling=tile."
complete -c swaylock-plugin -l version                -s v --description "Show the version number and quit."
