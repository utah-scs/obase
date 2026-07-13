/**
 * Copyright (c) 2012 YCSB contributors. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License. See accompanying
 * LICENSE file.
 */

/**
 * Redis client binding for YCSB.
 *
 * All YCSB records are mapped to a Redis *hash field*.  For scanning
 * operations, all keys are saved (by an arbitrary hash) in a sorted set.
 */


package site.ycsb.db;

import site.ycsb.ByteIterator;
import site.ycsb.DB;
import site.ycsb.DBException;
import site.ycsb.Status;
import site.ycsb.StringByteIterator;
import redis.clients.jedis.Jedis;
import redis.clients.jedis.JedisPooled;
import redis.clients.jedis.Connection;
import redis.clients.jedis.ConnectionFactory;
import redis.clients.jedis.DefaultJedisClientConfig;
import redis.clients.jedis.HostAndPort;
import redis.clients.jedis.JedisCluster;
import redis.clients.jedis.Protocol;
import redis.clients.jedis.exceptions.JedisConnectionException;
import org.newsclub.net.unix.AFUNIXSocket;
import org.newsclub.net.unix.AFUNIXSocketAddress;
import redis.clients.jedis.JedisSocketFactory;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.net.Socket;
import java.util.HashMap;
import java.util.Map;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Properties;
import java.util.Set;
import java.util.Vector;

/**
 * YCSB binding for <a href="http://redis.io/">Redis</a>.
 *
 * See {@code redis/README.md} for details.
 */
public class RedisClient extends DB {

  private Object jedisClient;

  public static final String HOST_PROPERTY = "redis.host";
  public static final String PORT_PROPERTY = "redis.port";
  public static final String PASSWORD_PROPERTY = "redis.password";
  public static final String CLUSTER_PROPERTY = "redis.cluster";
  public static final String TIMEOUT_PROPERTY = "redis.timeout";
  public static final String SOCKET_PATH_PROPERTY = "redis.socket.path";

  public static final String INDEX_KEY = "_indices";

private static class UdsJedisSocketFactory implements JedisSocketFactory {
    private final File socketFile;
    private final int timeout;
    private final DefaultJedisClientConfig config;

    public UdsJedisSocketFactory(String socketPath, int timeout, String password) {
        this.socketFile = new File(socketPath);
        this.timeout = timeout;
        DefaultJedisClientConfig.Builder builder = DefaultJedisClientConfig.builder()
            .timeoutMillis(timeout);
        if (password != null) {
            builder.password(password);
        }
        this.config = builder.build();
    }

    @Override
    public Socket createSocket() throws JedisConnectionException {
        try {
            Socket socket = AFUNIXSocket.newStrictInstance();
            socket.connect(new AFUNIXSocketAddress(socketFile), timeout);
            return socket;
        } catch (IOException ioe) {
            throw new JedisConnectionException("Failed to create UDS connection.", ioe);
        }
    }
}

public void init() throws DBException {
    Properties props = getProperties();
    String socketPath = props.getProperty(SOCKET_PATH_PROPERTY);
    String password = props.getProperty(PASSWORD_PROPERTY);
    
    if (socketPath != null && !socketPath.isEmpty()) {
        // Use Unix Domain Socket
        int timeout = Integer.parseInt(
            props.getProperty(TIMEOUT_PROPERTY, String.valueOf(Protocol.DEFAULT_TIMEOUT)));
        
        JedisSocketFactory socketFactory = new UdsJedisSocketFactory(socketPath, timeout, password);
        jedisClient = new Jedis(socketFactory);
        
    } else {
        // Use traditional TCP/IP connection
        int port = Integer.parseInt(
            props.getProperty(PORT_PROPERTY, String.valueOf(Protocol.DEFAULT_PORT)));
        String host = props.getProperty(HOST_PROPERTY);

        boolean clusterEnabled = Boolean.parseBoolean(props.getProperty(CLUSTER_PROPERTY));
        if (clusterEnabled) {
            Set<HostAndPort> jedisClusterNodes = new HashSet<>();
            jedisClusterNodes.add(new HostAndPort(host, port));
            jedisClient = new JedisCluster(jedisClusterNodes);
        } else {
            DefaultJedisClientConfig config = DefaultJedisClientConfig.builder()
                .password(password)
                .timeoutMillis(Integer.parseInt(
                    props.getProperty(TIMEOUT_PROPERTY, String.valueOf(Protocol.DEFAULT_TIMEOUT))))
                .build();
            jedisClient = new Jedis(host, port, config);
        }
    }
}

  public void cleanup() throws DBException {
    try {
      if (jedisClient instanceof Closeable) {
        ((Closeable) jedisClient).close();
      }
    } catch (IOException e) {
      throw new DBException("Closing connection failed.");
    }
  }

  /*
   * Calculate a hash for a key to store it in an index. The actual return value
   * of this function is not interesting -- it primarily needs to be fast and
   * scattered along the whole space of doubles. In a real world scenario one
   * would probably use the ASCII values of the keys.
   */
  private double hash(String key) {
    return key.hashCode();
  }

  private Object getRedisCommand() {
    if (jedisClient instanceof Jedis) {
      return jedisClient;
    } else if (jedisClient instanceof JedisPooled) {
      return jedisClient;
    } else {
      return jedisClient;
    }
  }

  // XXX jedis.select(int index) to switch to `table`

  @Override
  public Status read(String table, String key, Set<String> fields,
      Map<String, ByteIterator> result) {
    Object redis = getRedisCommand();
    
    if (fields == null) {
      Map<String, String> stringResult;
      if (redis instanceof Jedis) {
        stringResult = ((Jedis) redis).hgetAll(key);
      } else if (redis instanceof JedisPooled) {
        stringResult = ((JedisPooled) redis).hgetAll(key);
      } else {
        stringResult = ((JedisCluster) redis).hgetAll(key);
      }
      StringByteIterator.putAllAsByteIterators(result, stringResult);
    } else {
      String[] fieldArray = (String[]) fields.toArray(new String[fields.size()]);
      List<String> values;
      if (redis instanceof Jedis) {
        values = ((Jedis) redis).hmget(key, fieldArray);
      } else if (redis instanceof JedisPooled) {
        values = ((JedisPooled) redis).hmget(key, fieldArray);
      } else {
        values = ((JedisCluster) redis).hmget(key, fieldArray);
      }

      Iterator<String> fieldIterator = fields.iterator();
      Iterator<String> valueIterator = values.iterator();

      while (fieldIterator.hasNext() && valueIterator.hasNext()) {
        String val = valueIterator.next();
        if (val != null) {
          result.put(fieldIterator.next(), new StringByteIterator(val));
        }
        fieldIterator.next();
      }
    }
    return result.isEmpty() ? Status.ERROR : Status.OK;
  }

  @Override
  public Status insert(String table, String key,
      Map<String, ByteIterator> values) {
    Object redis = getRedisCommand();
    String status;
    if (redis instanceof Jedis) {
      status = ((Jedis) redis).hmset(key, StringByteIterator.getStringMap(values));
      ((Jedis) redis).zadd(INDEX_KEY, hash(key), key);
    } else if (redis instanceof JedisPooled) {
      status = ((JedisPooled) redis).hmset(key, StringByteIterator.getStringMap(values));
      ((JedisPooled) redis).zadd(INDEX_KEY, hash(key), key);
    } else {
      status = ((JedisCluster) redis).hmset(key, StringByteIterator.getStringMap(values));
      ((JedisCluster) redis).zadd(INDEX_KEY, hash(key), key);
    }
    return status.equals("OK") ? Status.OK : Status.ERROR;
  }

  @Override
  public Status delete(String table, String key) {
    Object redis = getRedisCommand();
    long deleteResult;
    long zremResult;
    if (redis instanceof Jedis) {
      deleteResult = ((Jedis) redis).del(key);
      zremResult = ((Jedis) redis).zrem(INDEX_KEY, key);
    } else if (redis instanceof JedisPooled) {
      deleteResult = ((JedisPooled) redis).del(key);
      zremResult = ((JedisPooled) redis).zrem(INDEX_KEY, key);
    } else {
      deleteResult = ((JedisCluster) redis).del(key);
      zremResult = ((JedisCluster) redis).zrem(INDEX_KEY, key);
    }
    return deleteResult == 0 && zremResult == 0 ? Status.ERROR : Status.OK;
  }

  @Override
  public Status update(String table, String key,
      Map<String, ByteIterator> values) {
    Object redis = getRedisCommand();
    String status;
    if (redis instanceof Jedis) {
      status = ((Jedis) redis).hmset(key, StringByteIterator.getStringMap(values));
    } else if (redis instanceof JedisPooled) {
      status = ((JedisPooled) redis).hmset(key, StringByteIterator.getStringMap(values));
    } else {
      status = ((JedisCluster) redis).hmset(key, StringByteIterator.getStringMap(values));
    }
    return status.equals("OK") ? Status.OK : Status.ERROR;
  }

@Override
public Status scan(String table, String startkey, int recordcount,
    Set<String> fields, Vector<HashMap<String, ByteIterator>> result) {
    Object redis = getRedisCommand();
    List<String> keysList;
    if (redis instanceof Jedis) {
        keysList = ((Jedis) redis).zrangeByScore(INDEX_KEY, hash(startkey),
            Double.POSITIVE_INFINITY, 0, recordcount);
    } else if (redis instanceof JedisPooled) {
        keysList = ((JedisPooled) redis).zrangeByScore(INDEX_KEY, hash(startkey),
            Double.POSITIVE_INFINITY, 0, recordcount);
    } else {
        keysList = ((JedisCluster) redis).zrangeByScore(INDEX_KEY, hash(startkey),
            Double.POSITIVE_INFINITY, 0, recordcount);
    }

    HashMap<String, ByteIterator> values;
    for (String key : keysList) {
        values = new HashMap<String, ByteIterator>();
        read(table, key, fields, values);
        result.add(values);
    }

    return Status.OK;
}
}