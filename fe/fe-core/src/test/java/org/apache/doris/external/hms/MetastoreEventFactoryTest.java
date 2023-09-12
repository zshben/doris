// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.external.hms;

import org.apache.doris.datasource.hive.event.AddPartitionEvent;
import org.apache.doris.datasource.hive.event.AlterDatabaseEvent;
import org.apache.doris.datasource.hive.event.AlterPartitionEvent;
import org.apache.doris.datasource.hive.event.AlterTableEvent;
import org.apache.doris.datasource.hive.event.CreateDatabaseEvent;
import org.apache.doris.datasource.hive.event.CreateTableEvent;
import org.apache.doris.datasource.hive.event.DropDatabaseEvent;
import org.apache.doris.datasource.hive.event.DropPartitionEvent;
import org.apache.doris.datasource.hive.event.DropTableEvent;
import org.apache.doris.datasource.hive.event.InsertEvent;
import org.apache.doris.datasource.hive.event.MetastoreEvent;
import org.apache.doris.datasource.hive.event.MetastoreEventFactory;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.google.common.collect.Sets;
import org.apache.commons.collections.CollectionUtils;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Random;
import java.util.Set;
import java.util.function.Function;


public class MetastoreEventFactoryTest {

    private static final MetastoreEventFactory factory = new MetastoreEventFactory();
    private static final Random random = new Random(System.currentTimeMillis());
    private static final String testCtl = "test_ctl";

    private static final Function<Long, CreateDatabaseEvent> createDatabaseEventProducer = eventId
                -> new CreateDatabaseEvent(eventId, testCtl, randomDb());

    private static final Function<Long, AlterDatabaseEvent> alterDatabaseEventProducer = eventId
                -> new AlterDatabaseEvent(eventId, testCtl, randomDb(), randomBool(0.0001D));

    private static final Function<Long, DropDatabaseEvent> dropDatabaseEventProducer = eventId
                -> new DropDatabaseEvent(eventId, testCtl, randomDb());

    private static final Function<Long, CreateTableEvent> createTableEventProducer = eventId
                -> new CreateTableEvent(eventId, testCtl, randomDb(), randomTbl());

    private static final Function<Long, AlterTableEvent> alterTableEventProducer = eventId
                -> new AlterTableEvent(eventId, testCtl, randomDb(), randomTbl(),
                randomBool(0.1D), randomBool(0.1D));

    private static final Function<Long, InsertEvent> insertEventProducer = eventId
                -> new InsertEvent(eventId, testCtl, randomDb(), randomTbl());

    private static final Function<Long, DropTableEvent> dropTableEventProducer = eventId
                -> new DropTableEvent(eventId, testCtl, randomDb(), randomTbl());

    private static final Function<Long, AddPartitionEvent> addPartitionEventProducer = eventId
                -> new AddPartitionEvent(eventId, testCtl, randomDb(), randomTbl(), randomPartitions());

    private static final Function<Long, AlterPartitionEvent> alterPartitionEventProducer = eventId
                -> new AlterPartitionEvent(eventId, testCtl, randomDb(), randomTbl(), randomPartition(),
                randomBool(0.1D));

    private static final Function<Long, DropPartitionEvent> dropPartitionEventProducer = eventId
                -> new DropPartitionEvent(eventId, testCtl, randomDb(), randomTbl(), randomPartitions());

    private static final List<Function<Long, ? extends MetastoreEvent>> eventProducers = Arrays.asList(
                createDatabaseEventProducer, alterDatabaseEventProducer, dropDatabaseEventProducer,
                createTableEventProducer, alterTableEventProducer, insertEventProducer, dropTableEventProducer,
                addPartitionEventProducer, alterPartitionEventProducer, dropPartitionEventProducer);

    private static String randomDb() {
        return "db_" + random.nextInt(10);
    }

    private static String randomTbl() {
        return "tbl_" + random.nextInt(100);
    }

    private static String randomPartition() {
        return "partition_" + random.nextInt(1000);
    }

    private static List<String> randomPartitions() {
        int times = random.nextInt(100) + 1;
        Set<String> partitions = Sets.newHashSet();
        for (int i = 0; i < times; i++) {
            partitions.add(randomPartition());
        }
        return Lists.newArrayList(partitions);
    }

    private static boolean randomBool(double possibility) {
        Preconditions.checkArgument(possibility >= 0.0D && possibility <= 1.0D);
        int upperBound = (int) Math.floor(1000000 * possibility);
        return random.nextInt(1000000) <= upperBound;
    }

    // define MockCatalog/MockDatabase/MockTable/MockPartition to simulate the real catalog/database/table/partition
    private static class MockCatalog {
        private String ctlName;
        private Map<String, MockDatabase> databases = Maps.newHashMap();

        private MockCatalog(String ctlName) {
            this.ctlName = ctlName;
        }

        @Override
        public int hashCode() {
            return 31 * Objects.hash(ctlName) + Arrays.hashCode(
                        databases.values().stream().sorted(Comparator.comparing(d -> d.dbName)).toArray());
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof MockCatalog)) {
                return false;
            }
            if (!Objects.equals(this.ctlName, ((MockCatalog) other).ctlName)) {
                return false;
            }
            Object[] sortedDatabases = databases.values().stream()
                        .sorted(Comparator.comparing(d -> d.dbName)).toArray();
            Object[] otherSortedDatabases = ((MockCatalog) other).databases.values().stream()
                        .sorted(Comparator.comparing(d -> d.dbName)).toArray();
            return Arrays.equals(sortedDatabases, otherSortedDatabases);
        }

        public MockCatalog copy() {
            MockCatalog mockCatalog = new MockCatalog(this.ctlName);
            mockCatalog.databases.putAll(this.databases);
            return mockCatalog;
        }
    }

    private static class MockDatabase {
        private String dbName;
        private Map<String, MockTable> tables = Maps.newHashMap();

        private MockDatabase(String dbName) {
            this.dbName = dbName;
        }

        @Override
        public int hashCode() {
            return 31 * Objects.hash(dbName) + Arrays.hashCode(
                        tables.values().stream().sorted(Comparator.comparing(t -> t.tblName)).toArray());
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof MockDatabase)) {
                return false;
            }
            if (!Objects.equals(this.dbName, ((MockDatabase) other).dbName)) {
                return false;
            }
            Object[] sortedTables = tables.values().stream()
                        .sorted(Comparator.comparing(t -> t.tblName)).toArray();
            Object[] otherSortedTables = ((MockDatabase) other).tables.values().stream()
                        .sorted(Comparator.comparing(t -> t.tblName)).toArray();
            return Arrays.equals(sortedTables, otherSortedTables);
        }

        public MockDatabase copy() {
            MockDatabase mockDatabase = new MockDatabase(this.dbName);
            mockDatabase.tables.putAll(this.tables);
            return mockDatabase;
        }
    }

    private static class MockTable {
        private String tblName;
        // use this filed to mark if the table has been refreshed
        private boolean refreshed;
        private Map<String, MockPartition> partitions = Maps.newHashMap();

        private MockTable(String tblName) {
            this.tblName = tblName;
        }

        public void refresh() {
            this.refreshed = true;
        }

        @Override
        public int hashCode() {
            return 31 * Objects.hash(tblName, refreshed) + Arrays.hashCode(
                        partitions.values().stream().sorted(Comparator.comparing(p -> p.partitionName)).toArray());
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof MockTable)) {
                return false;
            }
            if (!Objects.equals(this.tblName, ((MockTable) other).tblName)) {
                return false;
            }
            if (refreshed != ((MockTable) other).refreshed) {
                return false;
            }
            Object[] sortedPartitions = partitions.values().stream()
                        .sorted(Comparator.comparing(p -> p.partitionName)).toArray();
            Object[] otherSortedPartitions = ((MockTable) other).partitions.values().stream()
                        .sorted(Comparator.comparing(p -> p.partitionName)).toArray();
            return Arrays.equals(sortedPartitions, otherSortedPartitions);
        }

        public MockTable copy() {
            MockTable copyTbl = new MockTable(this.tblName);
            copyTbl.partitions.putAll(this.partitions);
            return copyTbl;
        }
    }

    private static class MockPartition {
        private String partitionName;
        // use this filed to mark if the partition has been refreshed
        private boolean refreshed;

        private MockPartition(String partitionName) {
            this.partitionName = partitionName;
            this.refreshed = false;
        }

        public void refresh() {
            this.refreshed = true;
        }

        @Override
        public int hashCode() {
            return Objects.hash(refreshed, partitionName);
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof MockPartition
                        && refreshed == ((MockPartition) other).refreshed
                        && Objects.equals(this.partitionName, ((MockPartition) other).partitionName);
        }
    }

    // simulate the processes when handling hms events
    private void processEvent(MockCatalog ctl, MetastoreEvent event) {
        switch (event.getEventType()) {

            case CREATE_DATABASE:
                MockDatabase database = new MockDatabase(event.getDbName());
                ctl.databases.put(database.dbName, database);
                break;

            case DROP_DATABASE:
                ctl.databases.remove(event.getDbName());
                break;

            case ALTER_DATABASE:
                String dbName = event.getDbName();
                if (((AlterDatabaseEvent) event).isRename()) {
                    ctl.databases.remove(dbName);
                    MockDatabase newDatabase = new MockDatabase(((AlterDatabaseEvent) event).getDbNameAfter());
                    ctl.databases.put(newDatabase.dbName, newDatabase);
                } else {
                    if (ctl.databases.containsKey(event.getDbName())) {
                        ctl.databases.get(event.getDbName()).tables.clear();
                    }
                }
                break;

            case CREATE_TABLE:
                if (ctl.databases.containsKey(event.getDbName())) {
                    MockTable tbl = new MockTable(event.getTblName());
                    ctl.databases.get(event.getDbName()).tables.put(event.getTblName(), tbl);
                }
                break;

            case DROP_TABLE:
                if (ctl.databases.containsKey(event.getDbName())) {
                    ctl.databases.get(event.getDbName()).tables.remove(event.getTblName());
                }
                break;

            case ALTER_TABLE:
            case INSERT:
                if (ctl.databases.containsKey(event.getDbName())) {
                    if (event instanceof AlterTableEvent
                                && (((AlterTableEvent) event).isRename() || ((AlterTableEvent) event).isView())) {
                        ctl.databases.get(event.getDbName()).tables.remove(event.getTblName());
                        MockTable tbl = new MockTable(((AlterTableEvent) event).getTblNameAfter());
                        ctl.databases.get(event.getDbName()).tables.put(tbl.tblName, tbl);
                    } else {
                        MockTable tbl = ctl.databases.get(event.getDbName()).tables.get(event.getTblName());
                        if (tbl != null) {
                            tbl.partitions.clear();
                            tbl.refresh();
                        }
                    }
                }
                break;

            case ADD_PARTITION:
                if (ctl.databases.containsKey(event.getDbName())) {
                    MockTable tbl = ctl.databases.get(event.getDbName()).tables.get(event.getTblName());
                    if (tbl != null) {
                        for (String partitionName : ((AddPartitionEvent) event).getAllPartitionNames()) {
                            MockPartition partition = new MockPartition(partitionName);
                            tbl.partitions.put(partitionName, partition);
                        }
                    }
                }
                break;

            case ALTER_PARTITION:
                if (ctl.databases.containsKey(event.getDbName())) {
                    MockTable tbl = ctl.databases.get(event.getDbName()).tables.get(event.getTblName());
                    AlterPartitionEvent alterPartitionEvent = ((AlterPartitionEvent) event);
                    if (tbl != null) {
                        if (alterPartitionEvent.isRename()) {
                            for (String partitionName : alterPartitionEvent.getAllPartitionNames()) {
                                tbl.partitions.remove(partitionName);
                            }
                            MockPartition partition = new MockPartition(alterPartitionEvent.getPartitionNameAfter());
                            tbl.partitions.put(partition.partitionName, partition);
                        } else {
                            for (String partitionName : alterPartitionEvent.getAllPartitionNames()) {
                                MockPartition partition = tbl.partitions.get(partitionName);
                                if (partition != null) {
                                    partition.refresh();
                                }
                            }
                        }
                    }
                }
                break;

            case DROP_PARTITION:
                if (ctl.databases.containsKey(event.getDbName())) {
                    MockTable tbl = ctl.databases.get(event.getDbName()).tables.get(event.getTblName());
                    if (tbl != null) {
                        for (String partitionName : ((DropPartitionEvent) event).getAllPartitionNames()) {
                            tbl.partitions.remove(partitionName);
                        }
                    }
                }
                break;

            default:
                Assertions.fail("Unknown event type : " + event.getEventType());
        }
    }

    static class EventProducer {
        // every type of event has a proportion
        // for instance, if the `CreateDatabaseEvent`'s proportion is 1
        // and the `AlterDatabaseEvent`'s proportion is 10
        // the event count of `AlterDatabaseEvent` is always about 10 times as the `CreateDatabaseEvent`
        private final List<Integer> proportions;
        private final int sumOfProportions;

        EventProducer(List<Integer> proportions) {
            Preconditions.checkArgument(CollectionUtils.isNotEmpty(proportions)
                        && proportions.size() == eventProducers.size());
            this.proportions = ImmutableList.copyOf(proportions);
            this.sumOfProportions = proportions.stream().mapToInt(proportion -> proportion).sum();
        }

        public MetastoreEvent produceOneEvent(long eventId) {
            return eventProducers.get(calIndex(random.nextInt(sumOfProportions))).apply(eventId);
        }

        private int calIndex(int val) {
            int currentIndex = 0;
            int currentBound = proportions.get(currentIndex);
            while (currentIndex < proportions.size() - 1) {
                if (val > currentBound) {
                    currentBound += proportions.get(++currentIndex);
                } else {
                    return currentIndex;
                }
            }
            return proportions.size() - 1;
        }
    }

    @Test
    public void testCreateBatchEvents() {
        AlterPartitionEvent e1 = new AlterPartitionEvent(1L, "test_ctl", "test_db", "t1", "p1", "p1");
        AlterPartitionEvent e2 = new AlterPartitionEvent(2L, "test_ctl", "test_db", "t1", "p1", "p1");
        AddPartitionEvent e3 = new AddPartitionEvent(3L, "test_ctl", "test_db", "t1", Arrays.asList("p1"));
        AlterTableEvent e4 = new AlterTableEvent(4L, "test_ctl", "test_db", "t1", false, false);
        AlterTableEvent e5 = new AlterTableEvent(5L, "test_ctl", "test_db", "t1", true, false);
        AlterTableEvent e6 = new AlterTableEvent(6L, "test_ctl", "test_db", "t1", false, true);
        DropTableEvent e7 = new DropTableEvent(7L, "test_ctl", "test_db", "t1");
        InsertEvent e8 = new InsertEvent(8L, "test_ctl", "test_db", "t1");
        CreateDatabaseEvent e9 = new CreateDatabaseEvent(9L, "test_ctl", "test_db2");
        AlterPartitionEvent e10 = new AlterPartitionEvent(10L, "test_ctl", "test_db", "t2", "p1", "p1");
        AlterTableEvent e11 = new AlterTableEvent(11L, "test_ctl", "test_db", "t1", false, false);
        CreateTableEvent e12 = new CreateTableEvent(12L, "test_ctl", "test_db", "t1");
        AlterDatabaseEvent e13 = new AlterDatabaseEvent(13L, "test_ctl", "test_db", true);
        AlterDatabaseEvent e14 = new AlterDatabaseEvent(14L, "test_ctl", "test_db", false);

        List<MetastoreEvent> mergedEvents;
        List<MetastoreEvent> testEvents = Lists.newArrayList();

        testEvents.add(e1);
        testEvents.add(e2);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 1);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 2L);

        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e3);
        testEvents.add(e9);
        testEvents.add(e10);
        testEvents.add(e4);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 3);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 9L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(2).getEventId() == 4L);

        // because e5 is a rename event, it will not be merged
        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e5);
        testEvents.add(e4);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 3);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 5L);
        Assertions.assertTrue(mergedEvents.get(2).getEventId() == 4L);

        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e6);
        testEvents.add(e4);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 3);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 6L);
        Assertions.assertTrue(mergedEvents.get(2).getEventId() == 4L);

        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e4);
        testEvents.add(e11);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 2);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 11L);

        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e4);
        testEvents.add(e8);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 2);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 8L);

        // because e5 is a rename event, it will not be merged
        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e5);
        testEvents.add(e8);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 3);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 5L);
        Assertions.assertTrue(mergedEvents.get(2).getEventId() == 8L);

        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e12);
        testEvents.add(e4);
        testEvents.add(e7);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 2);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 7L);

        // because e5 is a rename event, it will not be merged
        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e5);
        testEvents.add(e7);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 3);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 5L);
        Assertions.assertTrue(mergedEvents.get(2).getEventId() == 7L);

        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e4);
        testEvents.add(e13);
        testEvents.add(e7);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 4);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 4L);
        Assertions.assertTrue(mergedEvents.get(2).getEventId() == 13L);
        Assertions.assertTrue(mergedEvents.get(3).getEventId() == 7L);

        testEvents.clear();
        testEvents.add(e1);
        testEvents.add(e2);
        testEvents.add(e10);
        testEvents.add(e4);
        testEvents.add(e14);
        testEvents.add(e7);
        mergedEvents = factory.createBatchEvents("test_ctl", testEvents);
        Assertions.assertTrue(mergedEvents.size() == 3);
        Assertions.assertTrue(mergedEvents.get(0).getEventId() == 10L);
        Assertions.assertTrue(mergedEvents.get(1).getEventId() == 14L);
        Assertions.assertTrue(mergedEvents.get(2).getEventId() == 7L);
    }
}
