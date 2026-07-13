package site.ycsb.db;

import site.ycsb.ByteIterator;
import site.ycsb.DB;
import site.ycsb.DBException;
import site.ycsb.Status;
import site.ycsb.StringByteIterator;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.Vector;
import java.util.Properties;
import java.io.IOException;
import java.io.ByteArrayOutputStream;
import java.util.stream.IntStream;
import java.util.stream.Collectors;

/**
 * YCSB binding for CrestDB. Line-framed text protocol, strictly lockstep
 * (one command per round trip). Each YCSB field is stored as its own
 * key/value pair under the compound key "key:field". Values may contain
 * spaces but never '\n' (the framing delimiter).
 */
public class CrestClient extends DB {
  private static final String HOST_PROPERTY = "crest.host";
  private static final String PORT_PROPERTY = "crest.port";
  private static final int NUM_FIELDS = 10;
  private CrestConnection connection;
  private ByteBuffer readBuffer;

  @Override
  public void init() throws DBException {
    Properties props = getProperties();
    String host = props.getProperty(HOST_PROPERTY, "127.0.0.1");
    int port = Integer.parseInt(props.getProperty(PORT_PROPERTY, "6363"));
    String unixSocketPath = "/tmp/server.sock";

    try {
      connection = new CrestConnection(host, port, unixSocketPath);
      readBuffer = ByteBuffer.allocateDirect(1 << 16);
    } catch (Exception e) {
      throw new DBException("Could not open connection to Crest: " + e.getMessage(), e);
    }
  }

  @Override
  public void cleanup() throws DBException {
    try {
      if (connection != null) {
        connection.close();
      }
    } catch (IOException e) {
      throw new DBException("Could not close connection to Crest: " + e.getMessage(), e);
    }
  }

  /** Send one command line and read exactly one '\n'-terminated response. */
  private String roundTrip(String command) throws IOException {
    ByteBuffer writeBuf = ByteBuffer.wrap(command.getBytes(StandardCharsets.UTF_8));
    while (writeBuf.hasRemaining()) {
      connection.getSocketChannel().write(writeBuf);
    }

    ByteArrayOutputStream responseStream = new ByteArrayOutputStream(4096);
    while (true) {
      readBuffer.clear();
      int bytesRead = connection.getSocketChannel().read(readBuffer);
      if (bytesRead < 0) {
        throw new IOException("connection closed by server");
      }
      if (bytesRead == 0) {
        continue;
      }
      readBuffer.flip();
      byte[] chunk = new byte[readBuffer.remaining()];
      readBuffer.get(chunk);
      // The protocol is lockstep: the '\n' can only be the last byte of
      // this response, so checking the chunk tail is sufficient.
      responseStream.write(chunk);
      if (chunk[chunk.length - 1] == '\n') {
        break;
      }
    }
    String resp = new String(responseStream.toByteArray(), StandardCharsets.UTF_8);
    // strip trailing newline (values may legitimately end in spaces)
    return resp.substring(0, resp.length() - 1);
  }

  private Set<String> generateDefaultFields() {
    return IntStream.range(0, NUM_FIELDS)
        .mapToObj(i -> "field" + i)
        .collect(Collectors.toSet());
  }

  @Override
  public Status read(String table, String key, Set<String> fields, Map<String, ByteIterator> result) {
    try {
      Set<String> targetFields = (fields == null) ? generateDefaultFields() : fields;
      for (String field : targetFields) {
        String response = roundTrip("GET " + key + ":" + field + "\n");
        if (response.equals("ERR")) {
          return Status.NOT_FOUND;
        }
        result.put(field, new StringByteIterator(response));
      }
      return Status.OK;
    } catch (Exception e) {
      e.printStackTrace();
      return Status.ERROR;
    }
  }

  @Override
  public Status insert(String table, String key, Map<String, ByteIterator> values) {
    return upsert(key, values);
  }

  @Override
  public Status update(String table, String key, Map<String, ByteIterator> values) {
    return upsert(key, values);
  }

  private Status upsert(String key, Map<String, ByteIterator> values) {
    try {
      for (Map.Entry<String, ByteIterator> entry : values.entrySet()) {
        String response = roundTrip("SET " + key + ":" + entry.getKey() + " " + entry.getValue().toString() + "\n");
        if (!response.equals("OK")) {
          return Status.ERROR;
        }
      }
      return Status.OK;
    } catch (Exception e) {
      e.printStackTrace();
      return Status.ERROR;
    }
  }

  /**
   * Range scan. Records are contiguous runs of compound keys sharing the
   * "key:" prefix, so scanning recordcount*NUM_FIELDS pairs from
   * "startkey:field0" covers recordcount records. The server response is
   * length-prefixed ("OK <n> <klen> <vlen> <key> <value> ...") because
   * values contain spaces; parse by offsets, never by tokens.
   */
  @Override
  public Status scan(String table, String startkey, int recordcount, Set<String> fields,
                     Vector<HashMap<String, ByteIterator>> result) {
    try {
      int npairs = recordcount * NUM_FIELDS;
      String response = roundTrip("SCAN " + startkey + ":field0 " + npairs + "\n");
      if (response.startsWith("ERR")) {
        // unordered structure (hash table): scans unsupported
        return Status.NOT_IMPLEMENTED;
      }
      if (!response.startsWith("OK ")) {
        return Status.ERROR;
      }

      int pos = 3; // past "OK "
      int sp = response.indexOf(' ', pos);
      int count = Integer.parseInt(sp < 0 ? response.substring(pos) : response.substring(pos, sp));
      pos = (sp < 0) ? response.length() : sp + 1;

      String currentRecord = null;
      HashMap<String, ByteIterator> record = null;
      for (int i = 0; i < count; i++) {
        sp = response.indexOf(' ', pos);
        int klen = Integer.parseInt(response.substring(pos, sp));
        pos = sp + 1;
        sp = response.indexOf(' ', pos);
        int vlen = Integer.parseInt(response.substring(pos, sp));
        pos = sp + 1;
        String compound = response.substring(pos, pos + klen);
        pos += klen + 1;
        String value = response.substring(pos, pos + vlen);
        pos += vlen;
        if (pos < response.length() && response.charAt(pos) == ' ') {
          pos++;
        }

        int colon = compound.lastIndexOf(':');
        if (colon < 0) {
          continue; // not a compound key (e.g. a sentinel); skip
        }
        String rec = compound.substring(0, colon);
        String field = compound.substring(colon + 1);

        if (!rec.equals(currentRecord)) {
          if (result.size() >= recordcount) {
            break;
          }
          currentRecord = rec;
          record = new HashMap<>();
          result.add(record);
        }
        if (fields == null || fields.contains(field)) {
          record.put(field, new StringByteIterator(value));
        }
      }
      return Status.OK;
    } catch (Exception e) {
      e.printStackTrace();
      return Status.ERROR;
    }
  }

  @Override
  public Status delete(String table, String key) {
    try {
      // each field is its own KV pair; remove all of them
      boolean any = false;
      for (String field : generateDefaultFields()) {
        String response = roundTrip("DEL " + key + ":" + field + "\n");
        if (response.equals("OK")) {
          any = true;
        }
      }
      return any ? Status.OK : Status.NOT_FOUND;
    } catch (Exception e) {
      e.printStackTrace();
      return Status.ERROR;
    }
  }
}
