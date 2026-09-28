---
title: mdzy sample
tags: [markdown, test]
---

# mdzy feature tour

A paragraph with **bold**, *italic*, ***both***, ~~strikethrough~~, `inline code`,
a [link to GitHub](https://github.com/vladcherry/mdzy "title"), an autolink <https://example.com>,
a bare URL https://www.markdownguide.org and an email <someone@example.com>.
Line ending with two spaces  
forces a break. Entities: &copy; &mdash; &rarr; &#9733;. Escaped \*not italic\*, snake_case_word.

Кириллица тоже работает: **жирный**, *курсив*, `код`. Emoji: 🚀 ✅

## Lists

- First item
- Second item with a longer text that should wrap nicely onto the next line and keep the hanging indent aligned
  - Nested item
    - Deeper item
- [x] Done task
- [ ] Open task

1. One
2. Two
   ```js
   const x = 42; // code inside a list
   ```
3. Three

## Code

```c
#include <stdio.h>
/* block comment */
int main(void) {
    printf("Hello, %s!\n", "world"); // greet
    return 0x2A;
}
```

```python
def fib(n):
    # classic
    return n if n < 2 else fib(n - 1) + fib(n - 2)
```

    indented code block
    second line

## Quotes

> A blockquote with *emphasis* and a second paragraph.
>
> - a list inside
> - the quote
>
> > nested quote

> [!NOTE]
> Useful information that users should know.

> [!WARNING]
> Critical content demanding immediate user attention.

## Table

| Feature | Status | Size |
|:--------|:------:|-----:|
| Markdown | done | 1 |
| Tables with `code` and **bold** | done | 22 |
| Images | local only | 333 |

## Image

![mdzy icon](icon.png)

<p align="center">
  <img src="icon.png" width="64" alt="small icon">
</p>

## Links

- [Relative link to README](../README.md)
- [Anchor to the table](#table)
- Footnote reference[^1].

Setext heading
--------------

***

Final paragraph.

[^1]: This is the footnote text.
