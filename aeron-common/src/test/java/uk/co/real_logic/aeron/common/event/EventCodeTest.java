package uk.co.real_logic.aeron.common.event;

import org.junit.Test;

import java.util.HashSet;
import java.util.Set;

import static org.junit.Assert.assertTrue;

public class EventCodeTest
{
    @Test
    public void allTagsBitsAreUnique()
    {
        final Set<Long> seenTagBits = new HashSet<>();
        for (EventCode code : EventCode.values())
        {
            assertTrue(seenTagBits.add(code.tagBit()));
        }
    }
}
