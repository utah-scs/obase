## Crest DB
Prereq: >= Java 16
Add the following to: ~/.m2/settings.xml
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

`mvn -pl site.ycsb:crest-binding -am clean package -Dcheckstyle.skip`

Todo:
1. Scan workload
