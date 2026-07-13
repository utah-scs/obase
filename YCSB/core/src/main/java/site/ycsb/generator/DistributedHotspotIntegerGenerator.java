package site.ycsb.generator;

import java.util.Random;
import java.util.concurrent.ThreadLocalRandom;

public class DistributedHotspotIntegerGenerator extends NumberGenerator {

    private final long lowerBound;
    private final long upperBound;
    private final long[] hotKeys;
    private final double hotsetFraction;
    private final double hotOpnFraction;
    private final Random random;

    public DistributedHotspotIntegerGenerator(long lowerBound, long upperBound,
                                              double hotsetFraction, double hotOpnFraction) {
        if (hotsetFraction < 0.0 || hotsetFraction > 1.0) {
            throw new IllegalArgumentException("Hotset fraction must be between 0.0 and 1.0");
        }
        if (hotOpnFraction < 0.0 || hotOpnFraction > 1.0) {
            throw new IllegalArgumentException("Hot operation fraction must be between 0.0 and 1.0");
        }
        if (lowerBound > upperBound) {
            throw new IllegalArgumentException("Upper bound must be greater than or equal to lower bound");
        }

        this.lowerBound = lowerBound;
        this.upperBound = upperBound;
        this.hotsetFraction = hotsetFraction;
        this.hotOpnFraction = hotOpnFraction;
        // this.random = ThreadLocalRandom.current();
        this.random = new Random(42);  

        long keyRange = upperBound - lowerBound + 1;
        int hotKeyCount = (int)(keyRange * hotsetFraction);
        this.hotKeys = new long[hotKeyCount];

        // print number of selected hot keys
        System.out.println("Number of hot keys: " + hotKeyCount);

        // Select hot keys
        for (int i = 0; i < hotKeyCount; i++) {
            hotKeys[i] = lowerBound + Math.abs(random.nextLong()) % keyRange;
        }
    }

    /*
    * Only works for hotOpnFraction = 1.0
    */
    @Override
    public Long nextValue() {
        // Since hotOpnFraction is 1.0, we always choose from hot keys
        long value = hotKeys[random.nextInt(hotKeys.length)];
        setLastValue(value);
        return value;
    }

    @Override
    public double mean() {
        return (lowerBound + upperBound) / 2.0;
    }

    /**
     * @return the lowerBound
     */
    public long getLowerBound() {
        return lowerBound;
    }

    /**
     * @return the upperBound
     */
    public long getUpperBound() {
        return upperBound;
    }

    /**
     * @return the hotsetFraction
     */
    public double getHotsetFraction() {
        return hotsetFraction;
    }

    /**
     * @return the hotOpnFraction
     */
    public double getHotOpnFraction() {
        return hotOpnFraction;
    }
}