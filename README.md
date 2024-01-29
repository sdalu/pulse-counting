Conting pulse



| option | long name    | value                                                  | default          |
|--------|--------------|--------------------------------------------------------|------------------|
| -I     | idle-timeout | period value with suffix (`us`, `ms`, `s`, `min`, `h`) |                  |
| -D     | debounce     | period value with suffix (`us`, `ms`, `s`, `min`, `h`) |                  |
| -b     | bias         | `as-is`, `disabled`, `pull-up`, `pull-down`            | `as-is`          |
| -e     | edge         | `rising`, `falling`                                    | `rising`         |
| -L     | label        | string                                                 | `pulse-counting` |


~~~
pulse-counting rpi:39 -D 100us -I 1h -L water-meter -b pull-up -e falling
pulse-counting gpiochip0:26 -b pull-down -e rising
~~~
