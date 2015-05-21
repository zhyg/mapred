# mapred
Local MapReduce Program

## Usage
```
mapred [option] [INPUT]
option is :
 --mapper | -m the command to execute in shell
 --count  | -c the process num in background
 --help   | -h
 --version| -v
```

## Dependency
- libev(libev.a 4.15)

## Example
```
nohup cat input.txt | ./mapred -m "php test.php" -c 5 > result.txt 2>err &
```

