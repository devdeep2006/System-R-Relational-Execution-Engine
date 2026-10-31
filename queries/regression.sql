-- The original project's test cases (TC1-TC11), ported to the new engine.

-- Single-table selection and projection
SELECT * FROM students WHERE id = 5;                                   -- TC1
SELECT * FROM students WHERE age > 20 ORDER BY id LIMIT 5;             -- TC2
SELECT * FROM courses;                                                  -- TC3
SELECT * FROM courses WHERE id = 'CS2';                                 -- TC4

-- Selection push-down below a join
EXPLAIN SELECT * FROM students JOIN grades ON students.id = grades.student_id WHERE students.age = 18;   -- TC5
SELECT COUNT(*) FROM students JOIN grades ON students.id = grades.student_id WHERE students.age = 18;

-- Multi-way joins
SELECT COUNT(*) FROM students JOIN enrollments ON students.id = enrollments.student_id;                  -- TC6
EXPLAIN SELECT s.name, c.title
FROM students s JOIN enrollments e ON s.id = e.student_id JOIN courses c ON e.course_id = c.id;           -- TC7
EXPLAIN ANALYZE SELECT *
FROM students JOIN grades ON students.id = grades.student_id
              JOIN enrollments ON students.id = enrollments.student_id
              JOIN courses ON enrollments.course_id = courses.id
WHERE students.age > 18;                                                                                 -- TC8
SELECT COUNT(*) FROM students JOIN grades ON students.id = grades.student_id
                             JOIN enrollments ON students.id = enrollments.student_id
                             JOIN courses ON enrollments.course_id = courses.id
WHERE students.age > 18;

-- Aggregation
SELECT s.age, COUNT(*) AS n, AVG(g.grade) AS avg_grade, MAX(g.grade) AS best
FROM students s JOIN grades g ON s.id = g.student_id
GROUP BY s.age ORDER BY s.age;
