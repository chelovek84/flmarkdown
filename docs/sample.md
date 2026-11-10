# Field Notes

A short tour of what **flmarkdown** renders: *emphasis*, **strong text**, `inline code`, ~~strikethrough~~, and [links](https://github.com/chelovek84/flmarkdown) that open in your browser.

## Getting started

1. Install the two development packages.
2. Build with `make` and run:
   ```sh
   ./flmarkdown README.md
   ```
3. Press `Ctrl+T` to find the theme that suits your eyes.

- [x] Fast startup
- [x] Reloads when the file changes
- [ ] Syntax highlighting

> Good typography is invisible. The reader should notice the words,
> not the page they are printed on.

## Code

```c
#include <stdio.h>

int main(void) {
    printf("Hello, calm reader.\n");   /* no glare */
    return 0;
}
```

## Tables

| Theme | Background | Best for          |
|:------|:----------:|------------------:|
| Night | dark slate | evenings, low light |
| Paper | warm white | daylight reading  |
| Sepia | soft cream | long sessions     |

---

That's all. Jump back to [Getting started](#getting-started).
