Conting pulse



| option | long name    | value                                           |
|--------|--------------|-------------------------------------------------|
| -I     | idle-timeout | period value with suffix (`us`, `ms`, `s`, `min`, `h`) |
| -D     | debounce     | period value with suffix (`us`, `ms`, `s`, `min`, `h`) |
| -b     | bias         | `as-is`, `disabled`, `pull-up`, `pull-down`     |
| -e     | edge         | `rising`, `falling`                             |
| -L     | label        | string                                          |


~~~
pulse-counting -D 100us -I 1h -L water-meter -b pull-up -e falling
~~~