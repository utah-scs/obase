## Crest DB
Building and running the Crest binding requires JDK 16 or newer because it
uses Java's Unix domain socket APIs. On Ubuntu, install and select JDK 17:

```sh
sudo apt-get install openjdk-17-jdk-headless
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export PATH="$JAVA_HOME/bin:$PATH"
mvn -version
```

Confirm that `mvn -version` reports Java 17 (or another version >= 16).
Keep these environment variables set when running `bin/ycsb` as well.
If compilation reports missing `UnixDomainSocketAddress` or
`StandardProtocolFamily.UNIX`, Maven is using an older JDK.

If your network requires the UW CS proxy, add the following to
`~/.m2/settings.xml`:
```
<settings>
   <proxies>
        <proxy>
            <active>true</active>
            <protocol>http</protocol>
            <host>squid.cs.wisc.edu</host>
            <port>3128</port>
        </proxy>
    </proxies>
</settings>
```

From the `YCSB` directory, build the binding and its dependencies:

```sh
mvn -pl site.ycsb:crest-binding -am clean package -Dcheckstyle.skip
```

Todo:
1. Scan workload
