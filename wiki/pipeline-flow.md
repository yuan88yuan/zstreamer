**scheduler flow**
```
source thread:
    capture frame
    push downstream

encoder thread:
    pop queue
    encode
    push downstream

sink thread:
    write file
```

**pipeline 執行模型**

```
v4l2src
    -> queue
    -> h264enc
    -> queue
    -> mp4mux

alsasrc
    -> queue
    -> aacenc
    -> queue
    -> mp4mux
    -> queue
    -> filesink
```