# Audacity with cursor time in stdout

A very small change on the code to print the time of the cursor (float, in seconds) to stdout. Useful if want to use some video monitor (like xjadeo).

In order to activate this, enable (uncomment) the "experimental" option

```
PRINT_POSITION_TO_STDOUT
```

in src/Experimental.h

Also, it includes a Pro Tools theme for Audacity.
