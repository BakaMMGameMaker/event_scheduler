# README.md
1.tick 中只会处理 tick 之前就已经 schedule 的事件，tick 中 schedule 的事件至少等下一次 tick 才会触发
2.tick 中的 schedule 会等到本次 tick 之后才进入事件队列
3.tick 中的 clear 会让本 tick 中 clear 之前 schedule 的事件失效，但是不影响 clear 之后的 schedule
4.到达事件时间点但是仍未触发，且被 cancel 的事件不再触发
5.run 会按照事件时间推进，直到所有事件触发完毕
6.Repeat 事件的间隔不能为 0
7.如果设定事件的绝对触发时间 < now，那么事件会在下一次 tick 时触发
8.tick 中 clear 之前 schedule 的事件的 eid 是无效的
9.resume 会一次性 tick 已经暂停的时间
10.tick 中的 schedule 和 clear 操作会等到本次 tick 的末尾再一次性处理
11.tick 中回调抛出时，会立即执行所有抛出点前且在本 tick 内的 schedule 和 clear 操作
12.tick 中回调抛出时，如果抛出的事件为 Once，事件节点会被照常回收，如果为 Repeat，则重新调度
13.取消事件时，事件节点不会被立即回收，而是等到 tick 中遇到时懒回收
14.set_next_fire 和 delay 接口
15.clear 不会重置时间